//! Paired model-only probe. Both arms replay the same 313 training windows
//! and score the same chronological held-out sample using full-window argmax.
#[path = "../src/tinyseq.rs"]
mod tinyseq;
use std::{collections::HashMap, env, fs, hint::black_box, time::Instant};
use tinyseq::Model;

fn cpu_ns() -> u64 {
    let mut t = libc::timespec {tv_sec:0, tv_nsec:0};
    assert_eq!(unsafe {libc::clock_gettime(libc::CLOCK_THREAD_CPUTIME_ID, &mut t)}, 0);
    t.tv_sec as u64 * 1_000_000_000 + t.tv_nsec as u64
}

fn main() {
    let raw = fs::read(env::args().nth(1).expect("PSREC1 single-thread synth trace")).unwrap();
    assert!(raw.starts_with(b"PSREC1\n"));
    let mut rows = Vec::new();
    let mut pos = 7;
    while pos < raw.len() {
        let b = &raw[pos..];
        let n = match b[0] {
            b'E' => 33,
            b'A' => {
                rows.push((u64::from_le_bytes(b[1..9].try_into().unwrap()), u32::from_le_bytes(b[17..21].try_into().unwrap()),u64::from_le_bytes(b[9..17].try_into().unwrap()) / 4096));
                26
            },
            b'D' | b'P' => 25 + 16 * u32::from_le_bytes(b[21..25].try_into().unwrap()) as usize,
            tag => panic!("invalid record {tag} at {pos}"),
        };
        assert!(b.len() >= n);
        pos += n;
    }
    rows.sort_by_key(|r| r.0);
    let pid = rows.first().unwrap().1;
    assert!(rows.iter().all(|r| r.1 == pid), "single process required");
    let mut previous = HashMap::new();
    let mut context = [63u8;32];
    let mut samples = Vec::with_capacity(rows.len());
    for (i,pair) in rows.windows(2).enumerate() {
        let (_,_,v) = pair[0];
        let (_,_,next) = pair[1];
        let delta = next as i64 - v as i64;
        let succ = previous.get(&v).copied();
        let token = match delta { -31..=-1 => (delta+31) as u8, 1..=31 => (delta+30) as u8, _ if succ == Some(next) => 62, _ => 63 };
        if i >= 32 {samples.push((context, token, succ, v, next));}
        if next != v {previous.insert(v,next);}
        context.rotate_left(1); context[31]=token;
    }
    let split = samples.len()*2/3;
    assert!(split > 0 && split < samples.len());
    let mut rng = 777u64;
    let windows: [usize; 313] = std::array::from_fn(|_| {
        rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
        (rng % split as u64) as usize
    });
    for weight in [1.0, 0.25] {
        let mut model = Model::new(); // fixed initialization seed
        let start = cpu_ns();
        for &index in &windows {
            let (x, target, ..) = &samples[index];
            assert!(model.train_weighted_until(x, *target, weight, &mut || false));
        }
        evaluate(&model, weight, windows.len(), cpu_ns() - start, &samples[split..]);
    }
}

type Sample = ([u8; 32], u8, Option<u64>, u64, u64);
fn evaluate(model: &Model, weight: f32, updates: usize, train_ns: u64, held_out: &[Sample]) {
    let mut latency = Vec::new();
    let (mut issued,mut hits,mut base_issued,mut base_hits)=(0,0,0,0);
    let mut hidden = [[0.;32];2];
    for &token in &held_out[0].0[..31] { model.advance(&mut hidden,token); }
    let mut inc = Vec::new(); let mut agree = 0;
    let stride = (held_out.len()/2048).max(1);
    for (i,(x,_,succ,v,next)) in held_out.iter().enumerate() {
        let started = Instant::now();
        let incremental = model.advance(&mut hidden, x[31]);
        inc.push(started.elapsed().as_nanos());
        if i % stride != 0 {continue;}
        let t=Instant::now();
        let y=model.predict(black_box(x));
        latency.push(t.elapsed().as_nanos());
        agree += usize::from(y == incremental);
        let pred=match y {0..=30=>v.checked_add_signed(y as i64-31),31..=61=>v.checked_add_signed(y as i64-30),62=>*succ,_=>None};
        issued+=usize::from(pred.is_some()); hits+=usize::from(pred==Some(*next));
        base_issued+=usize::from(succ.is_some()); base_hits+=usize::from(*succ==Some(*next));
    }
    latency.sort_unstable(); let n=latency.len();
    inc.sort_unstable();
    println!("abstain_weight={weight} step_agree={agree}/{n} step_ns_p50={} p99={} over_10us={}/{}",inc[inc.len()/2],inc[inc.len()*99/100],inc.len()-inc.partition_point(|&v| v<=10000),inc.len());
    println!("abstain_weight={weight} param_object_bytes={} train_updates={updates} train_cpu_ms={:.2} test={n} trained_issued={issued} trained_correct={hits} trained_wrong={} trained_abstain={} latest_successor_issued={base_issued} latest_successor_correct={base_hits} infer_ns_p50={} p99={} max={}",std::mem::size_of::<Model>(),train_ns as f64/1e6,issued-hits,n-issued,latency[n/2],latency[n*99/100],latency[n-1]);
}
