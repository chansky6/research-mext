// Fault entry drives M0–M4; classified completions settle the prediction ledger.
use crate::m4::{M4, M4Stats};
use crate::event::{Access, AccessKind, Fault};
use std::collections::HashMap;

pub const N_MEMBERS: usize = 5;
pub const M_NULL: usize = 0;
pub const M_STRIDE: usize = 1;
pub const M_SUCCESSOR: usize = 2;
pub const M_COEVICT: usize = 3;
const M_TINYSEQ: usize = 4;
pub const NAMES: [&str; N_MEMBERS] = ["M0", "M1", "M2", "M3", "M4"];

const PAGE: u64 = 4096;

// ---------------------------------------------------------------------------
// M1 Stride:每 (tid) 记最近 4 次缺页访问的 VPN 差;连续 2 次相同步长 → 外推。
struct M1 {
    /// tid → 最近访问的 (vpn, 最近差值序列)
    hist: HashMap<u32, M1Hist>,
    max_tids: usize,
}

#[derive(Default)]
struct M1Hist {
    last_vpn: u64,
    deltas: [i64; 2], // 最近两次差
    armed: u8,        // 已记录的差值个数
}

impl M1 {
    fn new() -> M1 {
        M1 { hist: HashMap::new(), max_tids: 4096 }
    }

    fn observe(&mut self, a: &Fault) {
        let vpn = a.va / PAGE;
        let h = self.hist.entry(a.tid).or_default();
        if h.last_vpn != 0 {
            let d = vpn as i64 - h.last_vpn as i64;
            h.deltas[0] = h.deltas[1];
            h.deltas[1] = d;
            if h.armed < 2 {
                h.armed += 1;
            }
        }
        h.last_vpn = vpn;
        if self.hist.len() > self.max_tids {
            self.hist.clear(); // 极端线程数场景兜底
        }
    }

    /// 返回预测的绝对 va(可能为空)。步长外推 k=1..=d,夹在 ±1M 页内。
    fn predict(&self, a: &Fault, d: usize, out: &mut Vec<u64>) {
        let Some(h) = self.hist.get(&a.tid) else { return };
        if h.armed < 2 || h.deltas[0] == 0 || h.deltas[0] != h.deltas[1] {
            return;
        }
        let s = h.deltas[1];
        let vpn = a.va / PAGE;
        for k in 1..=d as i64 {
            let n = vpn as i64 + k * s;
            if n <= 0 || n > (1 << 46) {
                break;
            }
            out.push(n as u64 * PAGE);
        }
    }
}

// ---------------------------------------------------------------------------
// M2 Successor:FarSight future map 的用户态版。vpn → 最近 2 个后继(LRU 计数)。
struct SuccEntry {
    a: (u64, u16),
    b: (u64, u16), // a = 更近
}
impl SuccEntry {
    fn touch(&mut self, succ: u64) {
        if self.a.0 == succ {
            self.a.1 = self.a.1.saturating_add(1);
        } else if self.b.0 == succ {
            let e = self.b;
            self.b = self.a;
            self.a = (e.0, e.1.saturating_add(1));
        } else {
            self.b = self.a;
            self.a = (succ, 1);
        }
    }
}

struct M2 {
    succ: HashMap<u64, SuccEntry>,
    /// tid → 上一次访问 vpn(按线程分流,规格 §11)
    last: HashMap<u32, u64>,
    cap: usize,
}

impl M2 {
    fn new(cap: usize) -> M2 {
        M2 { succ: HashMap::new(), last: HashMap::new(), cap }
    }

    fn observe(&mut self, a: &Fault) {
        let vpn = a.va / PAGE;
        if let Some(&prev) = self.last.get(&a.tid) {
            if prev != vpn {
                self.succ.entry(prev).or_insert_with(|| SuccEntry { a: (vpn, 1), b: (0, 0) }).touch(vpn);
                if self.succ.len() > self.cap {
                    self.succ.clear(); // P2 简化:超限清表(冷启动重学)
                }
            }
        }
        self.last.insert(a.tid, vpn);
    }

    /// 沿后继链走 d 步(第一后继优先)
    fn predict(&self, a: &Fault, d: usize, out: &mut Vec<u64>) {
        let mut cur = a.va / PAGE;
        for _ in 0..d {
            let Some(e) = self.succ.get(&cur) else { break };
            let (next, _) = if e.a.1 >= e.b.1 { e.a } else { e.b };
            if next == 0 {
                break;
            }
            out.push(next * PAGE);
            cur = next;
        }
    }
}

// ---------------------------------------------------------------------------
// M3 Co-evict:同一 PAGEOUT 批次的相邻页成组;组内任一页缺页时预取其余页。
struct CoGroup {
    tgid: u32,
    pages: Vec<u64>, // 组内页 va(升序)
    count: u16, // 共访问率 ×65536,右移衰减
}

struct M3 {
    groups: Vec<CoGroup>,
    member: HashMap<(u32, u64), usize>, // (tgid, va) -> group idx
    cap_members: usize,
    max_group_pages: usize,
}

impl M3 {
    fn new() -> M3 {
        M3 { groups: Vec::new(), member: HashMap::new(), cap_members: 65536, max_group_pages: 4096 }
    }

    /// 控制器 PAGEOUT 批次回调注册(segs 为已换出段)
    fn register(&mut self, tgid: u32, segs: &[(u64, u64)]) {
        let mut pages = Vec::new();
        for &(s, l) in segs {
            let n = (l / PAGE) as usize;
            if pages.len() + n > self.max_group_pages {
                break;
            }
            for i in 0..n {
                pages.push(s + (i as u64) * PAGE);
            }
        }
        if pages.len() < 2 {
            return;
        }
        if self.member.len() + pages.len() > self.cap_members {
            // 淘汰最老组
            if let Some(g) = self.groups.first() {
                for &p in &g.pages {
                    self.member.remove(&(g.tgid, p));
                }
                self.groups.remove(0);
            }
        }
        let idx = self.groups.len();
        for &p in &pages {
            self.member.insert((tgid, p), idx);
        }
        self.groups.push(CoGroup { tgid, pages, count: 0 });
    }

    fn observe(&mut self, a: &Fault) {
        let Some(&gi) = self.member.get(&(a.tgid, a.va)) else { return };
        self.groups[gi].count = (self.groups[gi].count >> 1) | 0x8000; // 衰减后置 1
    }

    /// 组内成员缺页 → 预取其余页(上限 d 页,升序最近的优先)
    fn predict(&self, a: &Fault, d: usize, out: &mut Vec<u64>) {
        let Some(&gi) = self.member.get(&(a.tgid, a.va)) else { return };
        let g = &self.groups[gi];
        if g.count < 4096 {
            return; // 共访问率 < 1/16,组失效(规格 §5.2)
        }
        let i = match g.pages.binary_search(&a.va) {
            Ok(i) => i,
            Err(_) => return,
        };
        // 从缺页页向两侧扩展取 d 页
        let mut lo = i;
        let mut hi = i + 1;
        while out.len() < d {
            let take_lo = if lo > 0 { Some(lo - 1) } else { None };
            let take_hi = if hi < g.pages.len() { Some(hi) } else { None };
            match (take_lo, take_hi) {
                (Some(l), Some(h)) => {
                    if a.va - g.pages[l] <= g.pages[h] - a.va {
                        out.push(g.pages[l]);
                        lo -= 1;
                    } else {
                        out.push(g.pages[h]);
                        hi += 1;
                    }
                }
                (Some(l), None) => {
                    out.push(g.pages[l]);
                    lo -= 1;
                }
                (None, Some(h)) => {
                    out.push(g.pages[h]);
                    hi += 1;
                }
                (None, None) => break,
            }
        }
    }
}

// ---------------------------------------------------------------------------
// 影子账本 + 选择器
struct OpenPred {
    member: u8,
    t_pred: u64,
    dispatched: bool,
}

#[derive(Default, Clone)]
pub struct MemberStat {
    pub preds: u64,       // 影子预测总数
    pub hits: u64,        // W 内该页被访问(Hit/Late)
    pub address_matches: u64, // 窗口内同进程同页访问，含 Major
    pub wasted_dispatch: u64, // 已下发但 W 内未被访问 / 或访问为 Major
    pub dispatched: u64,
}

pub struct Ledger {
    open: HashMap<(u32, u64), Vec<OpenPred>>, // (tgid, va) -> 成员预测
    awaiting: HashMap<(u32,u32,u64,u64), Vec<OpenPred>>,
    in_flight: HashMap<(u32, u64), u64>,      // 已下发 -> ts
    pub window_ns: u64,
    pub lam: f64,     // λ:浪费代价
    pub eta: f64,     // Hedge 学习率
    pub epoch_utility: [f64; N_MEMBERS],
    pub stat: [MemberStat; N_MEMBERS],
    log_weights: [f64; N_MEMBERS],
    last_sweep_ns: u64,
    pub major_p90_ns: u64, // 最近 epoch 的 major io_wait p90(AIMD 输入)
    major_waits: Vec<u64>,
}

impl Ledger {
    pub fn new(window_ns: u64, lam: f64, eta: f64) -> Ledger {
        Ledger {
            open: HashMap::new(),
            awaiting: HashMap::new(),
            in_flight: HashMap::new(),
            window_ns,
            lam,
            eta,
            epoch_utility: [0.0; N_MEMBERS],
            stat: Default::default(),
            log_weights: [0.0; N_MEMBERS],
            last_sweep_ns: 0,
            major_p90_ns: 0,
            major_waits: Vec::new(),
        }
    }

    pub fn shadow_record(&mut self, tgid: u32, va: u64, member: usize, t_pred: u64) {
        let v = self.open.entry((tgid, va)).or_default();
        if v.iter().any(|p| p.member as usize == member) {
            return; // 同成员同页去重(窗口内)
        }
        v.push(OpenPred { member: member as u8, t_pred, dispatched: false });
        self.stat[member].preds += 1;
    }

    /// 派发:该页所有 open 预测标记 dispatched(credit 属于所有预测成员)
    pub fn mark_dispatched(&mut self, tgid: u32, va: u64, t: u64) {
        if let Some(v) = self.open.get_mut(&(tgid, va)) {
            for p in v.iter_mut() {
                if !p.dispatched {
                    p.dispatched = true;
                    self.stat[p.member as usize].dispatched += 1;
                }
            }
        }
        self.in_flight.insert((tgid, va), t);
    }

    pub fn in_flight(&self, tgid: u32, va: u64) -> bool {
        self.in_flight.contains_key(&(tgid, va))
    }

    pub fn fault_open(&mut self, a: &Fault) {
        let page = (a.tgid,a.va);
        self.in_flight.remove(&page);
        if let Some(v) = self.open.remove(&page) {
            self.awaiting.insert((a.tgid,a.tid,a.va,a.ts_ns),v);
        }
    }

    pub fn fault_closed(&mut self, a: &Access) {
        let start = a.ts_ns.saturating_sub(a.io_wait_ns);
        let v = self.awaiting.remove(&(a.tgid,a.tid,a.va,start));
        self.score(a, start, v);
    }

    /// 分类器访问落地:命中/迟到 → 记功;majflt → 已下发的算浪费
    pub fn on_access(&mut self, a: &Access) {
        // Chronological replay already has classified observations.
        let key = (a.tgid, a.va);
        self.in_flight.remove(&key);
        let v = self.open.remove(&key);
        self.score(a, a.ts_ns, v);
    }

    fn score(&mut self, a: &Access, start: u64, v: Option<Vec<OpenPred>>) {
        if a.kind == AccessKind::Major && a.io_wait_ns > 0 { self.major_waits.push(a.io_wait_ns); }
        let Some(v) = v else { return };
        let good = matches!(a.kind, AccessKind::Hit | AccessKind::Late);
        for p in v {
            let m = p.member as usize;
            let timely = start > p.t_pred && start - p.t_pred <= self.window_ns;
            if timely {
                self.stat[m].address_matches += 1;
            }
            if timely && good {
                self.stat[m].hits += 1;
                if p.dispatched && a.kind == AccessKind::Hit {
                    self.epoch_utility[m] += 1.0;
                }
            } else if p.dispatched {
                // 预测过且下发了,但访问仍 major:预取没赶上(误页/被逐出)
                self.stat[m].wasted_dispatch += 1;
                self.epoch_utility[m] -= self.lam;
            }
        }
    }

    /// 周期清理:超窗未访问的下发预测记浪费;AIMD 输入更新。
    /// 返回本 epoch 的 (utility, total_accesses) 供选择器。
    pub fn sweep(&mut self, now_ns: u64, total_accesses: u64) -> [f64; N_MEMBERS] {
        let w = self.window_ns;
        self.open.retain(|_, v| {
            v.retain(|p| {
                if now_ns.saturating_sub(p.t_pred) <= w {
                    true
                } else {
                    if p.dispatched {
                        self.stat[p.member as usize].wasted_dispatch += 1;
                        self.epoch_utility[p.member as usize] -= self.lam;
                    }
                    false
                }
            });
            !v.is_empty()
        });
        self.in_flight.retain(|_, t| now_ns.saturating_sub(*t) <= w);

        if !self.major_waits.is_empty() {
            self.major_waits.sort_unstable();
            self.major_p90_ns = self.major_waits[self.major_waits.len() * 9 / 10];
            self.major_waits.clear();
        }
        let u = self.epoch_utility;
        // Hedge 更新
        let n = total_accesses.max(1) as f64;
        for (m, utility) in u.iter().enumerate() {
            self.log_weights[m] += self.eta * utility / n;
        }
        self.epoch_utility = [0.0; N_MEMBERS];
        self.last_sweep_ns = now_ns;
        u
    }
}

// ---------------------------------------------------------------------------
// 组合引擎
pub struct Predictors {
    m1: M1,
    m2: M2,
    m3: M3,
    m4: Option<M4>,
    m4_proposed: bool,
    m4_selected: bool,
    io_rate: u64,
    pub only_member: Option<usize>, // §8.4 single-member ablation
    pub ledger: Ledger,
    pub lookahead: usize,
    /// M0 是否权重最高(= 本 epoch 禁止派发)
    pub dispatch_off: bool,
    /// 每 epoch 每 member 派发页数配额(选择器权重 × IO 速率),ingest 消耗
    pub quota: [f64; N_MEMBERS],
    pub member_used: [u64; N_MEMBERS],
}

impl Predictors {
    pub fn new(lookahead: usize, window_ns: u64, lam: f64, eta: f64, m2_cap: usize, m4: Option<M4>) -> Predictors {
        Predictors {
            m1: M1::new(),
            m2: M2::new(m2_cap),
            m3: M3::new(),
            m4,
            m4_proposed: false,
            m4_selected: false,
            io_rate: 0,
            only_member: None,
            ledger: Ledger::new(window_ns, lam, eta),
            lookahead,
            dispatch_off: false,
            quota: [0.0; N_MEMBERS],
            member_used: [0; N_MEMBERS],
        }
    }

    pub fn register_group(&mut self, tgid: u32, segs: &[(u64, u64)]) {
        self.m3.register(tgid, segs);
    }

    /// 分类器 Access 流入口:observe → 影子预测 → 返回本事件的派发候选
    /// (成员, va) 列表(去重、含所属成员;实际派发由调用方做配额/状态检查)。
    pub fn on_access(&mut self, a: &Access, now_ns: u64) -> Vec<(usize, u64)> {
        self.ledger.on_access(a);
        self.propose(&Fault::from(a), now_ns)
    }

    pub fn propose(&mut self, a: &Fault, now_ns: u64) -> Vec<(usize, u64)> {
        self.m1.observe(a);
        self.m2.observe(a);
        self.m3.observe(a);

        let d = self.lookahead;
        let mut cands: Vec<(usize, u64)> = Vec::new();
        let mut p = Vec::new();
        self.m1.predict(a, d, &mut p);
        for &va in &p {
            cands.push((M_STRIDE, va));
        }
        p.clear();
        self.m2.predict(a, d, &mut p);
        for &va in &p {
            cands.push((M_SUCCESSOR, va));
        }
        if let Some(m4) = &mut self.m4 {
            let successor = p.first().map(|va| va / PAGE);
            let mut tail = [0u64;3];
            let extra = d.saturating_sub(1).min(3);
            let vpn = m4.predict_pages(a.tgid, a.tid, a.va / PAGE, now_ns, successor, &mut tail[..extra]);
            if let Some(va) = vpn.and_then(|v| v.checked_mul(PAGE)).filter(|_| d > 0) {
                self.m4_proposed = true;
                cands.push((M_TINYSEQ, va));
                for &v in tail[..extra].iter().take_while(|&&v| v != 0) { cands.push((M_TINYSEQ,v * PAGE)); }
            }
            // A live overrun revokes the rest of this epoch's M4 quota immediately.
            if self.m4_selected && !m4.eligible(now_ns) {
                self.m4_selected = false;
                self.allocate_quota();
            }
        }
        p.clear();
        self.m3.predict(a, d, &mut p);
        for &va in &p {
            cands.push((M_COEVICT, va));
        }
        if let Some(member) = self.only_member { cands.retain(|&(m,_)| m == member); }
        // 影子记账(全部成员)
        let t = a.ts_ns;
        for &(m, va) in &cands {
            self.ledger.shadow_record(a.tgid, va, m, t);
        }
        cands.sort_unstable();
        cands.dedup();
        cands
    }

    /// 选择器 tick(1s):utility → 权重;M0 最高则关派发;配额按权重重算。
    /// io_rate_per_epoch = 当前 IO 令牌桶速率(页/epoch,epoch=1s)。
    pub fn epoch_tick(&mut self, now_ns: u64, total_accesses: u64, io_rate_per_epoch: u64) {
        let _ = self.ledger.sweep(now_ns, total_accesses);
        self.m4_selected = self.m4_proposed && self.m4.as_ref().is_some_and(|m| m.eligible(now_ns));
        self.m4_proposed = false;
        self.io_rate = io_rate_per_epoch;
        self.allocate_quota();
        self.member_used = [0; N_MEMBERS];
    }

    pub fn m4_stats(&self) -> Option<M4Stats> {
        self.m4.as_ref().map(M4::stats)
    }

    pub fn weights(&self) -> [f64; N_MEMBERS] {
        let mut w = self.ledger.log_weights;
        if !self.m4_selected {
            w[M_TINYSEQ] = f64::NEG_INFINITY;
        }
        let peak = w.iter().copied().fold(f64::NEG_INFINITY, f64::max);
        let w = w.map(|v| (v - peak).exp());
        let sum: f64 = w.iter().sum();
        w.map(|v| v / sum)
    }

    fn allocate_quota(&mut self) {
        if let Some(m) = self.only_member {
            self.quota.fill(0.0);
            self.dispatch_off = m == M_NULL || (m == M_TINYSEQ && !self.m4_selected);
            if !self.dispatch_off { self.quota[m] = self.io_rate as f64; }
            return;
        }
        let w = self.weights();
        // An idle member tied with M0 must not keep loss-making members issuing.
        // Newly validated M4 can probe when it first produces candidates.
        self.dispatch_off = !(1..N_MEMBERS).any(|m| self.ledger.stat[m].preds > 0 && w[m] >= w[M_NULL]);
        let sum_non_m0: f64 = (1..N_MEMBERS).map(|m| w[m]).sum();
        for m in 1..N_MEMBERS {
            self.quota[m] = if w[m] >= w[M_NULL] { w[m] / sum_non_m0.max(f64::MIN_POSITIVE) * self.io_rate as f64 } else { 0.0 };
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn cold_or_disabled_m4_preserves_four_member_allocation() {
        for m4 in [None, Some(M4::replay())] {
            let mut engine = Predictors::new(1, 100, 0.5, 0.5, 32, m4);
            let a = Access { tgid: 1, tid: 1, va: PAGE, ts_ns: 1, kind: AccessKind::Major, io_wait_ns: 0 };
            assert!(engine.on_access(&a, a.ts_ns).iter().all(|&(m, _)| m != M_TINYSEQ));
            engine.epoch_tick(2, 1, 300);
            assert_eq!(engine.weights(), [0.25, 0.25, 0.25, 0.25, 0.0]);
            assert_eq!(engine.quota, [0.0, 100.0, 100.0, 100.0, 0.0]);
            assert!(engine.dispatch_off);
        }
    }
}

#[cfg(test)]
mod delayed_feedback {
    use super::*;
    #[test]
    fn delayed_retry_cannot_reward_a_prediction_made_after_fault_entry() {
        let mut l = Ledger::new(1000,0.5,0.5);
        let mut a = Access { tgid:5, tid:7, va:PAGE, ts_ns:100, kind:AccessKind::Hit, io_wait_ns:0 };
        l.shadow_record(5,PAGE,M_STRIDE,80);
        l.mark_dispatched(5,PAGE,85);
        l.fault_open(&Fault::from(&a));
        l.shadow_record(5,PAGE,M_SUCCESSOR,110);
        a.ts_ns=130; a.io_wait_ns=30;
        l.fault_closed(&a);
        assert_eq!(l.epoch_utility[M_STRIDE],1.0);
        assert_eq!(l.stat[M_SUCCESSOR].address_matches,0);
        a.ts_ns=140; a.io_wait_ns=0;
        l.fault_open(&Fault::from(&a)); l.fault_closed(&a);
        assert_eq!(l.stat[M_SUCCESSOR].address_matches,1);
        assert_eq!(l.epoch_utility[M_SUCCESSOR],0.0);
    }
}
