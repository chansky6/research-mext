// 事件与访问分类(规格 v0.2 §4.1)
// 原始事件 = 32 字节,与 bpf/predswap.bpf.c struct swap_ev 一致。

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum EvKind {
    Swapin,
    Miss,
}

#[derive(Clone, Copy, Debug)]
pub struct SwapEv {
    pub ts_ns: u64,
    pub va: u64,
    pub tgid: u32,
    pub tid: u32,
    pub kind: EvKind,
}

pub const EV_SZ: usize = 32;

impl SwapEv {
    pub fn parse(b: &[u8; EV_SZ]) -> SwapEv {
        SwapEv {
            ts_ns: u64::from_le_bytes(b[0..8].try_into().unwrap()),
            va: u64::from_le_bytes(b[8..16].try_into().unwrap()),
            tgid: u32::from_le_bytes(b[16..20].try_into().unwrap()),
            tid: u32::from_le_bytes(b[20..24].try_into().unwrap()),
            kind: if b[24] == 0 { EvKind::Swapin } else { EvKind::Miss },
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum AccessKind {
    /// 及时命中:单次 do_swap_page,无 miss(预取或残留 cache)
    Hit,
    /// 真读盘:do_swap_page + swapin_readahead,通常伴随 RETRY 重试
    Major,
    /// 无 miss 但有间隔明显的重试对:预取在途,缺页在等页锁
    Late,
}

/// 一次访问(合并 RETRY 后)
#[derive(Clone, Copy, Debug)]
pub struct Access {
    pub ts_ns: u64,
    pub va: u64,
    pub tgid: u32,
    #[allow(dead_code)] // P2:多线程分流预测要用
    pub tid: u32,
    pub kind: AccessKind,
    /// 重试对间隔(≈ IO 等待);无重试为 0
    pub io_wait_ns: u64,
}

const RETRY_WIN_NS: u64 = 50_000_000; // RETRY 重试配对窗口
const LATE_WAIT_NS: u64 = 200_000; // 无 miss 重试对超过此时长判 late

#[derive(Clone, Copy)]
struct Pending {
    t_open: u64,
    has_miss: bool,
    tgid: u32,
}

/// miss 配对分类器。fed() 原始事件;poll(t) 关闭过期 pending;drain() 取出访问。
pub struct Classifier {
    pend: std::collections::HashMap<(u32, u64), Pending>,
    out: Vec<Access>,
    /// 最近一次访问时间(供冷却期查询):(tgid, va) -> ts
    pub last_access: std::collections::HashMap<(u32, u64), u64>,
}

impl Classifier {
    pub fn new() -> Classifier {
        Classifier {
            pend: std::collections::HashMap::new(),
            out: Vec::new(),
            last_access: std::collections::HashMap::new(),
        }
    }

    pub fn feed(&mut self, ev: &SwapEv) {
        let key = (ev.tid, ev.va);
        match ev.kind {
            EvKind::Swapin => {
                match self.pend.get(&key) {
                    // RETRY 重试对:关闭前一次访问
                    Some(p) if ev.ts_ns.saturating_sub(p.t_open) <= RETRY_WIN_NS => {
                        let wait = ev.ts_ns.saturating_sub(p.t_open);
                        let kind = if p.has_miss {
                            AccessKind::Major
                        } else if wait > LATE_WAIT_NS {
                            AccessKind::Late
                        } else {
                            AccessKind::Hit
                        };
                        self.pend.remove(&key);
                        self.push_access(*ev, kind, wait);
                    }
                    _ => {
                        // 悬挂 pending 超窗未重试:按 sweep 规则补关(理论少见:两次访问
                        // 落在同一 50ms 窗口且前次无重试)。先补关再开新的。
                        if let Some(p) = self.pend.remove(&key) {
                            let kind = if p.has_miss { AccessKind::Major } else { AccessKind::Hit };
                            self.push_ts(p.t_open, p.tgid, key, kind, 0);
                        }
                        self.pend.insert(key, Pending { t_open: ev.ts_ns, has_miss: false, tgid: ev.tgid });
                    }
                }
            }
            EvKind::Miss => {
                if let Some(p) = self.pend.get_mut(&key) {
                    p.has_miss = true;
                }
                // 无 pending 的 miss(理论上不存在):丢弃,sweep 会兜住关联访问
            }
        }
    }

    fn push_ts(&mut self, ts: u64, tgid: u32, key: (u32, u64), kind: AccessKind, wait: u64) {
        self.last_access.insert((tgid, key.1), ts);
        self.out.push(Access {
            ts_ns: ts,
            va: key.1,
            tgid,
            tid: key.0,
            kind,
            io_wait_ns: wait,
        });
    }

    fn push_access(&mut self, ev: SwapEv, kind: AccessKind, wait: u64) {
        self.last_access.insert((ev.tgid, ev.va), ev.ts_ns);
        self.out.push(Access {
            ts_ns: ev.ts_ns,
            va: ev.va,
            tgid: ev.tgid,
            tid: ev.tid,
            kind,
            io_wait_ns: wait,
        });
    }

    /// 关闭超过 RETRY 窗口的 pending(无重试 = 单事件访问)
    pub fn poll(&mut self, now_ns: u64) {
        let pend = &mut self.pend;
        let mut closed: Vec<((u32, u64), Pending)> = Vec::new();
        pend.retain(|k, p| {
            if now_ns.saturating_sub(p.t_open) > RETRY_WIN_NS {
                closed.push((*k, *p));
                false
            } else {
                true
            }
        });
        for (k, p) in closed {
            let kind = if p.has_miss { AccessKind::Major } else { AccessKind::Hit };
            self.last_access.insert((p.tgid, k.1), p.t_open);
            self.out.push(Access {
                ts_ns: p.t_open,
                va: k.1,
                tgid: p.tgid,
                tid: k.0,
                kind,
                io_wait_ns: 0,
            });
        }
    }

    pub fn drain(&mut self) -> Vec<Access> {
        std::mem::take(&mut self.out)
    }

}
