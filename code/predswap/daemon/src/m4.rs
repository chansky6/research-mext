//! TinySeq runtime (spec 5.2). Call `predict_pages` once per first fault,
//! even when the selector gives M4 no quota. Pass VPNs and the contemporaneous
//! M2 successor. The result and optional tail are candidates, not dispatches.
//!
//! `new` trains off the ingest thread. Starting from the first observation,
//! at most once per 60 s, a warm stream hands off a uniform reservoir of at
//! most 8192 labeled windows. The worker reserves a quarter for evaluation,
//! trains a private GRU on the rest (abstain loss weight 0.25), and groups its
//! full-context argmax outputs by two-token suffix. A unique plurality must
//! match at least 80% of at least eight held-out next tokens, counting abstain
//! targets in the denominator. Ingest does at most four indexed lookups,
//! never a full GRU forward pass or an unvalidated fallback by default.
//! `PREDSWAP_M4_SUFFIX=0` restores unweighted SGD + the exact-context cache;
//! `PREDSWAP_M4_INCREMENTAL=1` retains the unweighted incremental experiment.
//!
//! `replay` is deterministic on an ordered trace: same labels, warm-up,
//! cache-only prediction, sampling, and trace-time round boundaries, but
//! up to 32 SGD steps per round and no timing vetoes.
//! Replay is an accuracy experiment, not a live CPU/latency measurement.
//! `m2_only` / `m2_only_replay` replace the learned decision with token 62
//! unconditionally; warm-up, context length and rate gates still apply.
use crate::tinyseq::Model;
use std::collections::{HashMap, VecDeque};
use std::mem;
use std::sync::{mpsc, Arc};
use std::thread;
use std::time::{Duration, Instant};

const SECOND: u64 = 1_000_000_000;
const ROUND_NS: u64 = 60 * SECOND;
const WARMUP: u64 = 50_000;
const RATE: usize = 20_000;
const STREAM_CAP: usize = 4096;
const ROUND_CAP: usize = 8192;
const CACHE_CAP: usize = 256;
const REPLAY_STEPS: usize = 32;
const TRAIN_NS: u64 = 170_000_000;
const NOMINATE_NS: u64 = 192_000_000;
const CACHE_NS: u64 = 195_000_000;
const CPU_NS: u64 = 200_000_000;
const INFER_NS: u64 = 10_000;
const M2_TOKEN: u8 = 62;
const ABSTAIN: u8 = 63;
const MAX_VPN: u64 = u64::MAX / 4096;
type Context = [u8; 32];
// Mix neither processes nor unrelated 2 MiB neighborhoods into one delta stream.
const REGION_SHIFT: u32 = 9;
type Key = (u32, u32, u64);
// Diagnostic codes outside the model's 0..64 token vocabulary; never dispatched.
const LOW_SUPPORT: u8 = 64;
const DISAGREE: u8 = 65;
const UNSEEN: u8 = 66;
const VOCAB: usize = 64;
const SUFFIX_COUNT: usize = VOCAB * VOCAB;
const MIN_SUPPORT: u16 = 8;
type SuffixTable = Box<[u8; SUFFIX_COUNT]>;

fn suffix(ctx: &Context) -> usize { usize::from(ctx[30]) * VOCAB + usize::from(ctx[31]) }

#[derive(Debug, Default, Clone, Copy)]
pub struct DistillStats {
    /// Disjoint evaluation windows, including abstain targets and unmatched suffixes.
    pub eval_windows: u64,
    /// Nominated suffixes by outcome: unseen (always 0), sparse, rejected, abstain, usable.
    pub entries: [u64; 5],
    /// Held-out evidence for accepted entries only (not prospective accuracy).
    pub accepted_support: u64,
    pub accepted_correct: u64,
}

/// Monotonic totals unless prefixed `last_`, `max_`, or described as a gauge.
#[derive(Debug, Default, Clone, Copy)]
pub struct M4Stats {
    pub events: u64,
    /// Full, causally labeled training windows seen, before reservoir sampling.
    pub windows: u64,
    pub evictions: u64,
    pub out_of_order: u64,
    pub rounds: u64,
    pub trained_samples: u64,
    pub last_round_samples: u64,
    /// Actual worker-thread CPU time, including cache construction; live only.
    pub last_cpu_ns: u64,
    pub max_cpu_ns: u64,
    pub budget_stops: u64,
    /// Observed CPU violations, never hidden by clipping the measurement.
    /// A violating generation is not published.
    pub cpu_overruns: u64,
    pub worker_errors: u64,
    pub inferences: u64,
    pub max_infer_ns: u64,
    pub overruns: u64,
    pub sampled_out: u64,
    pub suppressed: u64,
    pub cache_hits: u64,
    pub cache_misses: u64,
    /// Distinct full contexts scored by the GRU for the published generation.
    pub cache_entries: u64,
    /// Completed training steps' labels, by [delta, M2, abstain].
    pub last_sgd_targets: [u64; 3],
    /// Full-context GRU outputs, by [delta, M2, abstain].
    pub last_teacher_outputs: [u64; 3],
    pub last_distill: DistillStats,
    /// Default-mode queries: unseen, insufficient support, rejected, abstain, usable.
    pub suffix_queries: [u64; 5],
}

fn encode(prev: u64, vpn: u64, saved_m2: Option<u64>) -> u8 {
    match i128::from(vpn) - i128::from(prev) {
        d @ -31..=-1 => (d + 31) as u8,
        d @ 1..=31 => (d + 30) as u8,
        _ if saved_m2 == Some(vpn) => M2_TOKEN,
        _ => ABSTAIN,
    }
}

fn decode(token: u8, vpn: u64, m2: Option<u64>) -> Option<u64> {
    match token {
        0..=30 => vpn.checked_sub(u64::from(31 - token)),
        31..=61 => vpn.checked_add(u64::from(token - 30)),
        M2_TOKEN => m2,
        _ => None,
    }.filter(|v| *v <= MAX_VPN)

}

fn suffix_bin(code: u8) -> usize {
    match code { UNSEEN => 0, LOW_SUPPORT => 1, DISAGREE => 2, ABSTAIN => 3, _ => 4 }
}

// Only teacher predictions nominate a token; held-out labels can veto but
// never choose an alternative. Tied pluralities (including ties with 63)
// abstain. Use every occurrence in the holdout for precision, not just hits.
fn distill(labels: &HashMap<Context, u8>, evaluation: &[Window], stop: &mut impl FnMut() -> bool)
    -> Option<(SuffixTable, DistillStats)>
{
    let mut votes = HashMap::new();
    for (ctx, &token) in labels {
        votes.entry(suffix(ctx)).or_insert_with(|| [0u16; VOCAB])[usize::from(token)] += 1;
    }
    let mut table = Box::new([UNSEEN; SUFFIX_COUNT]);
    for (&key, counts) in &votes {
        let max = counts.iter().max().unwrap();
        let mut winners = counts.iter().enumerate().filter(|(_, n)| *n == max);
        let token = winners.next().unwrap().0 as u8;
        table[key] = if winners.next().is_some() { DISAGREE } else { token };
    }
    let mut evidence = [(0u16, 0u16); SUFFIX_COUNT]; // round cap fits u16
    for batch in evaluation.chunks(64) {
        if stop() { return None; } // never accept on a truncated evaluation
        for w in batch {
            let key = suffix(&w.context);
            evidence[key].0 += 1;
            evidence[key].1 += u16::from(table[key] == w.target);
        }
    }
    let mut stats = DistillStats { eval_windows: evaluation.len() as u64, ..Default::default() };
    for key in votes.keys().copied() {
        let (total, correct) = evidence[key];
        let token = &mut table[key];
        if *token < ABSTAIN {
            if total < MIN_SUPPORT { *token = LOW_SUPPORT; }
            else if 5 * u32::from(correct) < 4 * u32::from(total) { *token = DISAGREE; }
            else {
                stats.accepted_support += u64::from(total);
                stats.accepted_correct += u64::from(correct);
            }
        }
        stats.entries[suffix_bin(*token)] += 1;
    }
    (!stop()).then_some((table, stats))
}

#[derive(Clone, Copy)]
struct Window { context: Context, target: u8 }

#[derive(Default)]
struct Stream {
    context: Context,
    len: usize,
    previous: Option<u64>,
    saved_m2: Option<u64>,
    ts: u64,
    hidden: [[f32; 32]; 2],
    generation: Option<u64>,
}

impl Stream {
    // The context is the prefix ending at `previous`, NOT at `vpn`.
    fn advance(&mut self, vpn: u64, m2: Option<u64>, ts: u64, record: impl FnOnce(&Context, u8)) {
        if let Some(prev) = self.previous {
            let token = encode(prev, vpn, self.saved_m2);
            if self.len == self.context.len() { record(&self.context, token); }
            self.context.copy_within(1.., 0);
            self.context[31] = token;
            self.len = (self.len + 1).min(32);
        }
        self.previous = Some(vpn);
        self.saved_m2 = m2;
        self.ts = ts;
    }
}

struct Streams {
    table: HashMap<Key, Stream>,
    fifo: VecDeque<Key>,
}

impl Streams {
    fn get(&mut self, key: Key, evictions: &mut u64) -> &mut Stream {
        if !self.table.contains_key(&key) {
            if self.fifo.len() == STREAM_CAP {
                self.table.remove(&self.fifo.pop_front().unwrap());
                *evictions += 1;
            }
            self.fifo.push_back(key);
        }
        self.table.entry(key).or_default()
    }
}

// Fixed seed, independent of pid hashing or the system entropy source.
struct Rng(u64);
impl Rng {
    fn next(&mut self) -> u64 {
        self.0 ^= self.0 << 13;
        self.0 ^= self.0 >> 7;
        self.0 ^= self.0 << 17;
        self.0
    }
}

struct Reservoir {
    windows: Vec<Window>,
    seen: u64,
    rng: Rng,
}

impl Reservoir {
    fn put(&mut self, context: &Context, target: u8) {
        self.seen += 1;
        if self.windows.len() < ROUND_CAP {
            self.windows.push(Window { context: *context, target });
        } else {
            let at = self.rng.next() % self.seen;
            if at < ROUND_CAP as u64 {
                self.windows[at as usize] = Window { context: *context, target };
            }
        }
    }
}

// Exactly the last RATE arrivals suffice to detect >RATE in a trailing second.
struct RateGate { arrivals: Box<[u64]>, cursor: usize }
impl RateGate {
    fn keep(&mut self, ts: u64, events: u64) -> bool {
        let high = events > RATE as u64 && ts - self.arrivals[self.cursor] < SECOND;
        self.arrivals[self.cursor] = ts;
        self.cursor = (self.cursor + 1) % RATE;
        !high || events % 8 == 0
    }
}

enum Cache { Exact(HashMap<Context, u8>), Distilled(SuffixTable) }
struct Snapshot { model: Model, cache: Cache }

#[derive(Default)]
struct Round {
    snapshot: Option<Arc<Snapshot>>,
    samples: u64,
    contexts: u64,
    sgd_targets: [u64; 3],
    teacher_outputs: [u64; 3],
    distill: DistillStats,
    cpu_ns: u64,
    budget_stop: bool,
    clock_error: bool,
}

fn thread_cpu_ns() -> Option<u64> {
    let mut t = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    // SAFETY: t is a writable timespec and is read only on success.
    if unsafe { libc::clock_gettime(libc::CLOCK_THREAD_CPUTIME_ID, &mut t) } != 0 { return None; }
    Some(t.tv_sec as u64 * SECOND + t.tv_nsec as u64)
}

fn fit(base: Option<&Snapshot>, windows: &mut [Window], seed: u64, live: bool, validated: bool) -> Round {
    let mut out = Round::default();
    let start = if live {
        let Some(start) = thread_cpu_ns() else { out.clock_error = true; return out };
        start
    } else { 0 };
    // Leave time after SGD for teacher inference, validation and bookkeeping.
    // A broken CPU clock fails closed.
    let mut clock_error = false;
    let mut expired = |limit| live && match thread_cpu_ns().and_then(|t| t.checked_sub(start)) {
        Some(ns) => ns >= limit,
        None => { clock_error = true; true }
    };
    let mut model = base.map_or_else(Model::new, |s| s.model.clone());
    let mut rng = Rng(seed);
    // The reservoir is uniform once full. Draw the evaluation subset without
    // replacement even before it fills, when its physical order is chronological.
    // It is never used for SGD or teacher nomination in this generation.
    let holdout = if validated { windows.len() / 4 } else { 0 };
    for i in 0..holdout {
        let j = i + (rng.next() % (windows.len() - i) as u64) as usize;
        windows.swap(i, j);
    }
    let (evaluation, windows) = windows.split_at_mut(holdout);
    let steps = if live { windows.len() } else { windows.len().min(REPLAY_STEPS) };
    let teacher_limit = if validated { NOMINATE_NS } else { CACHE_NS };
    let category = |t: u8| usize::from(t >= M2_TOKEN) + usize::from(t == ABSTAIN);
    for i in 0..steps {
        // Partial Fisher-Yates: random order, no duplicate sample indices.
        let j = i + (rng.next() % (windows.len() - i) as u64) as usize;
        windows.swap(i, j);
        let w = &windows[i];
        let mut stop = || expired(TRAIN_NS);
        let completed = if validated { model.train_weighted_until(&w.context, w.target, 0.25, &mut stop) }
                        else { model.train_until(&w.context, w.target, &mut stop) };
        if !completed {
            out.budget_stop = true;
            break;
        }
        out.samples += 1;
        for &t in w.context[1..].iter().chain(std::iter::once(&w.target)) { out.sgd_targets[category(t)] += 1; }
    }
    if out.samples > 0 {
        let mut labels = HashMap::with_capacity(CACHE_CAP);
        for w in windows.iter().rev() {
            if labels.len() == CACHE_CAP { break; }
            if expired(teacher_limit) { out.budget_stop = true; break; }
            if let std::collections::hash_map::Entry::Vacant(slot) = labels.entry(w.context) {
                let Some(label) = model.predict_until(&w.context, &mut || expired(teacher_limit)) else {
                    out.budget_stop = true;
                    break;
                };
                slot.insert(label);
                out.teacher_outputs[category(label)] += 1;
            }
        }
        out.contexts = labels.len() as u64;
        if validated {
            if let Some((table, stats)) = distill(&labels, evaluation, &mut || expired(CACHE_NS)) {
                out.distill = stats;
                out.snapshot = Some(Arc::new(Snapshot { model, cache: Cache::Distilled(table) }));
            } else { out.budget_stop = true; }
        } else if !labels.is_empty() {
            out.snapshot = Some(Arc::new(Snapshot { model, cache: Cache::Exact(labels) }));
        }
    }
    if live {
        match thread_cpu_ns().and_then(|t| t.checked_sub(start)) {
            Some(ns) => out.cpu_ns = ns,
            None => clock_error = true,
        }
        out.clock_error = clock_error;
        if clock_error || out.cpu_ns > CPU_NS { out.snapshot = None; }
    }
    out
}

struct Job { windows: Vec<Window>, base: Option<Arc<Snapshot>>, seed: u64, validated: bool }
type Finished = (Round, Vec<Window>);

struct Worker {
    tx: Option<mpsc::SyncSender<Job>>,
    rx: mpsc::Receiver<Finished>,
    handle: Option<thread::JoinHandle<()>>,
    spare: Vec<Window>,
    pending: bool,
    not_before: Instant,
}

impl Worker {
    fn new() -> std::io::Result<Self> {
        let (tx, requests) = mpsc::sync_channel::<Job>(1);
        let (results, rx) = mpsc::sync_channel(1);
        let handle = thread::Builder::new().name("predswap-m4".into()).spawn(move || {
            while let Ok(mut job) = requests.recv() {
                let round = fit(job.base.as_deref(), &mut job.windows, job.seed, true, job.validated);
                job.windows.clear();
                if results.send((round, job.windows)).is_err() { break; }
            }
        })?;
        Ok(Self { tx: Some(tx), rx, handle: Some(handle), spare: Vec::with_capacity(ROUND_CAP),
                  pending: false, not_before: Instant::now() + Duration::from_nanos(ROUND_NS) })
    }
}

impl Drop for Worker {
    fn drop(&mut self) {
        // At most one outstanding job and one result slot, so send never
        // requires an ingest wakeup to finish shutdown.
        drop(self.tx.take());
        if let Some(handle) = self.handle.take() { let _ = handle.join(); }
    }
}

enum Training { Worker(Worker), Replay, Disabled }

pub struct M4 {
    training: Training,
    m2_only: bool,
    incremental: bool,
    suffix: bool,
    rollout: bool,
    live: bool,
    streams: Streams,
    reservoir: Reservoir,
    rate: RateGate,
    active: Option<Arc<Snapshot>>,
    now: u64,
    next_round: Option<u64>,
    muted_epoch: Option<u64>,
    stats: M4Stats,
}

impl M4 {
    /// Live trained member; a failed worker start leaves it ineligible.
    pub fn new() -> Self { Self::make(true, false) }
    /// Offline chronological trace, deterministic step count instead of CPU time.
    pub fn replay() -> Self { Self::make(false, false) }
    /// Same live gates, but always use M2's current first successor. No SGD.
    pub fn m2_only() -> Self { Self::make(true, true) }
    /// Deterministic offline counterpart to `m2_only`.
    pub fn m2_only_replay() -> Self { Self::make(false, true) }

    fn make(live: bool, m2_only: bool) -> Self {
        let mut stats = M4Stats::default();
        let training = if m2_only { Training::Disabled } else if !live { Training::Replay } else {
            match Worker::new() {
                Ok(w) => Training::Worker(w),
                Err(_) => { stats.worker_errors += 1; Training::Disabled }
            }
        };
        let incremental = std::env::var_os("PREDSWAP_M4_INCREMENTAL").as_deref() == Some(std::ffi::OsStr::new("1"));
        let capacity = if matches!(&training, Training::Disabled) { 0 } else { ROUND_CAP };
        Self { training, live, m2_only, stats, incremental,
            rollout: std::env::var_os("PREDSWAP_M4_ROLLOUT").as_deref() == Some(std::ffi::OsStr::new("1")),
            suffix: !incremental && std::env::var_os("PREDSWAP_M4_SUFFIX").as_deref() != Some(std::ffi::OsStr::new("0")),
            streams: Streams { table: HashMap::new(), fifo: VecDeque::new() },
            reservoir: Reservoir { windows: Vec::with_capacity(capacity), seen: 0,
                                   rng: Rng(0x243f_6a88_85a3_08d3) },
            rate: RateGate { arrivals: vec![0; RATE].into_boxed_slice(), cursor: 0 },
            active: None, now: 0, next_round: None, muted_epoch: None }
    }

    /// Global selector gate. A particular stream may still lack a full window,
    /// be sampled out, miss the published table, or predict an abstention.
    pub fn eligible(&self, ts: u64) -> bool {
        self.stats.events > WARMUP && (self.m2_only || self.active.is_some())
            && (self.m2_only || !self.suffix || self.stats.last_distill.entries[4] != 0)
            && self.muted_epoch != Some(self.now.max(ts) / SECOND)
    }

    pub fn stats(&self) -> M4Stats { self.stats }

    fn publish(&mut self, r: Round) {
        self.stats.rounds += 1;
        self.stats.last_round_samples = r.samples;
        self.stats.trained_samples += r.samples;
        self.stats.last_cpu_ns = r.cpu_ns;
        self.stats.max_cpu_ns = self.stats.max_cpu_ns.max(r.cpu_ns);
        self.stats.budget_stops += u64::from(r.budget_stop);
        self.stats.cpu_overruns += u64::from(r.cpu_ns > CPU_NS);
        self.stats.worker_errors += u64::from(r.clock_error);
        self.stats.last_sgd_targets = r.sgd_targets;
        self.stats.last_teacher_outputs = r.teacher_outputs;
        if let Some(s) = r.snapshot {
            self.stats.cache_entries = r.contexts;
            self.stats.last_distill = r.distill;
            self.active = Some(s);
        }
    }

    fn service_training(&mut self) {
        if let Training::Worker(w) = &mut self.training {
            if w.pending {
                match w.rx.try_recv() {
                    Ok((r, spare)) => { w.pending = false; w.spare = spare; self.publish(r); }
                    Err(mpsc::TryRecvError::Empty) => return,
                    Err(mpsc::TryRecvError::Disconnected) => {
                        self.stats.worker_errors += 1;
                        self.training = Training::Disabled;
                        return;
                    }
                }
            }
        }
        let next = self.next_round.get_or_insert_with(|| self.now.saturating_add(ROUND_NS));
        if self.now < *next || self.stats.events <= WARMUP || self.reservoir.windows.is_empty() { return; }
        match &mut self.training {
            Training::Worker(w) => {
                let now = Instant::now();
                if now < w.not_before { return; } // also cap real frequency if ingest is catching up
                let job = Job { windows: mem::replace(&mut self.reservoir.windows, mem::take(&mut w.spare)),
                                base: self.active.clone(), seed: self.reservoir.rng.next(), validated: self.suffix };
                if w.tx.as_ref().unwrap().try_send(job).is_err() {
                    self.stats.worker_errors += 1;
                    self.training = Training::Disabled;
                    return;
                }
                w.pending = true;
                w.not_before = now + Duration::from_nanos(ROUND_NS);
            }
            Training::Replay => {
                let r = fit(self.active.as_deref(), &mut self.reservoir.windows, self.reservoir.rng.next(), false, self.suffix);
                self.reservoir.windows.clear();
                self.publish(r);
            }
            Training::Disabled => return,
        }
        self.reservoir.seen = 0;
        self.next_round = Some(self.now.saturating_add(ROUND_NS));
    }

    fn within_inference_budget(&mut self, epoch: u64, elapsed_ns: u64) -> bool {
        self.stats.max_infer_ns = self.stats.max_infer_ns.max(elapsed_ns);
        if elapsed_ns <= INFER_NS { return true; }
        self.stats.overruns += 1;
        self.muted_epoch = Some(epoch);
        false
    }

    /// `ts` is monotonic event time in nanoseconds. Interleaved processes and
    /// threads never share a prefix; a backwards timestamp within one stream
    /// is ignored. FIFO eviction restarts only the evicted stream. The first
    /// 50,000 observations never issue candidates, but do supply warm-up data.
    #[cfg(test)]
    pub fn observe_predict(&mut self, tgid: u32, tid: u32, vpn: u64, ts: u64, successor: Option<u64>) -> Option<u64> {
        self.predict_pages(tgid, tid, vpn, ts, successor, &mut [])
    }

    /// First demand candidate and up to three additional validated same-region deltas.
    /// A zero slot terminates the tail (VPN zero is not an agent target).
    pub fn predict_pages(&mut self, tgid: u32, tid: u32, vpn: u64, ts: u64, successor: Option<u64>, tail: &mut [u64]) -> Option<u64> {
        self.stats.events += 1;
        self.now = self.now.max(ts);
        let keep = self.rate.keep(self.now, self.stats.events);
        let training = !matches!(&self.training, Training::Disabled);
        if training { self.service_training(); }
        let regional = self.suffix && !self.m2_only;
        let region = if regional { vpn >> REGION_SHIFT } else { 0 };
        let s = self.streams.get((tgid, tid, region), &mut self.stats.evictions);
        if ts < s.ts { self.stats.out_of_order += 1; return None; }
        s.advance(vpn, successor, ts, |ctx, target| {
            if training {
                self.reservoir.put(ctx, target);
                self.stats.windows += 1;
            }
        });
        if self.stats.events <= WARMUP || s.len < 32 || (!self.m2_only && self.active.is_none()) { return None; }
        let epoch = self.now / SECOND;
        if self.muted_epoch == Some(epoch) { s.generation = None; self.stats.suppressed += 1; return None; }
        if !keep { s.generation = None; self.stats.sampled_out += 1; return None; }
        let start = self.live.then(Instant::now);
        let token = if self.m2_only { M2_TOKEN } else if self.incremental {
            if s.generation != Some(self.stats.rounds) {
                s.hidden = [[0.; 32]; 2];
                s.generation = Some(self.stats.rounds);
            }
            self.active.as_ref()?.model.advance(&mut s.hidden, s.context[31])
        } else {
            let snap = self.active.as_ref()?;
            let token = match &snap.cache {
                Cache::Exact(labels) => labels.get(&s.context).copied(),
                Cache::Distilled(table) => {
                    let code = table[suffix(&s.context)];
                    self.stats.suffix_queries[suffix_bin(code)] += 1;
                    (code < ABSTAIN).then_some(code)
                }
            };
            match token {
                Some(t) => { self.stats.cache_hits += 1; t }
                None => { self.stats.cache_misses += 1; ABSTAIN }
            }
        };
        let result = decode(token, vpn, successor).filter(|&v| !regional || v >> REGION_SHIFT == region);
        if let Some(mut page) = result.filter(|_| self.rollout && token < M2_TOKEN && regional && !self.incremental) {
            if let Cache::Distilled(table) = &self.active.as_ref()?.cache {
                let (mut prev, mut cur) = (s.context[31],token);
                for slot in tail.iter_mut().take(3) {
                    let t = table[usize::from(prev)*VOCAB + usize::from(cur)];
                    if t >= M2_TOKEN { break; }
                    let Some(next) = decode(t,page,None) else { break };
                    if next == 0 || next == vpn || next >> REGION_SHIFT != region { break; }
                    *slot = next; page = next; prev = cur; cur = t;
                }
            }
        }
        self.stats.inferences += 1;
        if let Some(start) = start {
            if !self.within_inference_budget(epoch, start.elapsed().as_nanos() as u64) { return None; }
        }
        result
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn train_on_previous_prefix_using_the_m2_answer_saved_then() {
        let mut s = Stream::default();
        // 32 +1 tokens. The successor saved at VPN 32 is the distant 100.
        for i in 0..=32 {
            s.advance(i, (i == 32).then_some(100), i, |_, _| panic!("short context"));
        }
        let mut rows = Vec::new();
        s.advance(100, Some(100), 33, |x, y| rows.push((*x, y)));
        s.advance(200, Some(200), 34, |x, y| rows.push((*x, y)));
        assert_eq!(rows[0], ([31; 32], 62));
        // Current successor equals the observed VPN, but the one saved for
        // the previous access did not predict it. Do not relabel as 62.
        let mut second = [31; 32];
        second[31] = 62;
        assert_eq!(rows[1], (second, 63));
    }

    #[test]
    fn token_endpoints_and_no_wrapping_vpns() {
        for (next, token) in [(69, 0), (99, 30), (101, 31), (131, 61)] {
            assert_eq!(encode(100, next, Some(next)), token);
            assert_eq!(decode(token, 100, None), Some(next));
        }
        assert_eq!(encode(0, u64::MAX, None), 63);
        assert_eq!(decode(30, 0, None), None);
        assert_eq!(decode(31, MAX_VPN, None), None);
        assert_eq!(decode(62, 50, None), None);
        assert_eq!(decode(63, 50, Some(123)), None);
    }

    #[test]
    fn separated_streams_and_bounded_eviction_restart_context() {
        let mut m = M4::replay();
        for i in 0..33 {
            for key in [(1, 1), (1, 2), (2, 1)] {
                m.observe_predict(key.0, key.1, i, i, None);
            }
        }
        assert_eq!(m.stats().windows, 0);
        m.observe_predict(1, 1, 500, 33, None);
        assert_eq!(m.stats().windows, 1);
        for i in 0..STREAM_CAP { m.observe_predict(3, i as u32, 100, 34, None); }
        assert_eq!(m.stats().evictions, 3);
        m.observe_predict(1, 1, 501, 35, None);
        assert_eq!(m.stats().windows, 1);
    }

    #[test]
    fn ablation_warmup_rate_sampling_and_epoch_reset() {
        let mut m = M4::m2_only_replay();
        for i in 0..WARMUP {
            assert_eq!(m.observe_predict(1, 1, i, 0, Some(42)), None);
        }
        assert!(!m.eligible(0));
        let emitted: Vec<_> = (0..16).filter_map(|i| m.observe_predict(1, 1, i, 1, Some(42))).collect();
        assert_eq!(emitted, [42, 42]);
        assert_eq!(m.stats().sampled_out, 14);
        assert_eq!(m.stats().windows, 0);
        assert!(m.within_inference_budget(0, 10_000));
        assert!(!m.within_inference_budget(0, 10_001));
        assert!(!m.eligible(SECOND - 1));
        assert_eq!(m.observe_predict(1, 1, 0, SECOND - 1, Some(42)), None);
        assert_eq!(m.stats().suppressed, 1);
        assert!(m.eligible(2 * SECOND));
        assert_eq!(m.observe_predict(1, 1, 1, 2 * SECOND, Some(71)), Some(71));
        assert_eq!(m.observe_predict(1, 1, 2, 2 * SECOND + 1, None), None);
    }

    #[test]
    fn suffix_predictions_require_independent_next_token_evidence() {
        let mut labels = HashMap::new();
        let mut evaluation = Vec::new();
        for (suffix, teacher, actual) in [
            (1, 31, &[31,31,31,31,31,31,31,31,63,62][..]), // 80% correct
            (2, 31, &[31,31,31,31,31,31,31][..]),         // too few, even at 100%
            (3, 31, &[31,31,31,31,31,31,31,63,63,63][..]), // abstentions count as errors
            (4, 63, &[31; 10][..]),                         // no GRU nomination
        ] {
            let mut context = [0; 32];
            context[31] = suffix;
            labels.insert(context, teacher);
            context[0] = 1; // never in the teacher cache
            evaluation.extend(actual.iter().map(|&target| Window { context, target }));
        }
        let (table, stats) = distill(&labels, &evaluation, &mut || false).unwrap();
        assert_eq!(&table[..6], &[UNSEEN, 31, LOW_SUPPORT, DISAGREE, ABSTAIN, UNSEEN]);
        assert_eq!(stats.accepted_correct, 8);
        assert_eq!(stats.accepted_support, 10);
        assert!(distill(&labels, &evaluation, &mut || true).is_none());
    }

    #[test]
    fn replay_generalizes_a_validated_suffix_to_a_new_full_context() {
        let mut m = M4::replay();
        m.suffix = true;
        m.incremental = false;
        m.rollout = true;
        for i in 0..WARMUP { m.observe_predict(1, 1, i, i, None); }
        let mut tail = [0;3];
        assert_eq!(m.predict_pages(1, 1, WARMUP, ROUND_NS, None, &mut tail), Some(WARMUP + 1));
        assert_eq!(tail,[WARMUP+2,WARMUP+3,WARMUP+4]);
        let st = m.stats();
        assert_eq!(st.last_distill.eval_windows, ROUND_CAP as u64 / 4);
        assert!(st.last_distill.accepted_correct > 0);
        assert_eq!(st.last_distill.accepted_correct, st.last_distill.accepted_support);
        assert_eq!(st.last_sgd_targets, [32 * REPLAY_STEPS as u64, 0, 0]);
        // The new -1 is not in the teacher cache. After two +1 tokens,
        // only the suffix (not the full window) is familiar again.
        assert_eq!(m.observe_predict(1, 1, WARMUP - 1, ROUND_NS + 1, None), None);
        assert_eq!(m.observe_predict(1, 1, WARMUP, ROUND_NS + 2, None), None);
        assert_eq!(m.observe_predict(1, 1, WARMUP + 1, ROUND_NS + 3, None), Some(WARMUP + 2));
        assert_eq!(m.stats().suffix_queries, [2, 0, 0, 0, 2]);
    }

    #[test]
    fn replay_publishes_only_exact_contexts_from_the_completed_round() {
        let mut m = M4::replay();
        m.suffix = false; // opt-out: old unweighted, exact-cache comparison
        m.incremental = false;
        for i in 0..WARMUP { m.observe_predict(1, 1, i, i, None); }
        assert_eq!(m.stats().rounds, 0);
        m.observe_predict(1, 1, WARMUP, ROUND_NS, None);
        let st = m.stats();
        assert_eq!((st.rounds, st.trained_samples, st.cache_entries, st.cache_hits), (1, 32, 1, 1));
        assert!(m.eligible(ROUND_NS));
        // The new -1 token cannot be present in the preceding +1-only cache.
        assert_eq!(m.observe_predict(1, 1, WARMUP - 1, ROUND_NS + 1, Some(123)), None);
        assert_eq!(m.stats().cache_misses, 1);
    }
}
