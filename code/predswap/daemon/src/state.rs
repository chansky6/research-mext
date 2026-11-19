// 共享状态:每 tgid 的区域页表(年龄/换出/待卸载/最近 swap-in),供分类器、
// 扫描线程、卸载控制器共享(规格 §4.2/§4.3/§6)。
use crate::event::AccessKind;
use std::collections::HashMap;

#[derive(Clone)]
pub struct RegionState {
    pub start: u64,
    pub end: u64,
    pub npages: usize,
    /// 距最近访问的空闲扫描周期数(0 = 上一周期被触碰)
    pub age: Vec<u32>,
    /// 换出位图
    pub swapped: Vec<u64>,
    /// 已 PAGEOUT 待对账(避免重复选中)
    pub offl_pending: Vec<u64>,
    /// 最近一次 swap-in 的 epoch 秒(0 = 从未,冷却期用)
    pub last_swapin: Vec<u32>,
    /// 上轮扫描 resident 页索引(空闲 PFN 跟踪的有效域)
    pub resident: Vec<u32>,
}

impl RegionState {
    fn new(start: u64, end: u64) -> RegionState {
        let np = ((end - start) / 4096) as usize;
        RegionState {
            start,
            end,
            npages: np,
            age: vec![0; np],
            swapped: vec![0; np.div_ceil(64)],
            offl_pending: vec![0; np.div_ceil(64)],
            last_swapin: vec![0; np],
            resident: Vec::new(),
        }
    }
    pub fn is_swapped(&self, i: usize) -> bool {
        self.swapped[i / 64] & (1 << (i % 64)) != 0
    }
    pub fn is_offl_pending(&self, i: usize) -> bool {
        self.offl_pending[i / 64] & (1 << (i % 64)) != 0
    }
}

pub struct ProcState {
    pub pid: u32,
    pub regions: Vec<RegionState>,
    pub rss_kb: u64,
    pub swap_kb: u64,
}

impl ProcState {
    pub fn va_index(&self, va: u64) -> Option<(usize, usize)> {
        // 区域数量少,线性;[start,end) 页对齐
        for (ri, r) in self.regions.iter().enumerate() {
            if r.start <= va && va < r.end {
                let i = ((va - r.start) / 4096) as usize;
                if i < r.npages {
                    return Some((ri, i));
                }
            }
        }
        None
    }
}

/// tgid 变更(增/退出)与区域刷新:重建区域页表时保留旧年龄(按 va 对齐)。
pub struct StateMap {
    pub procs: HashMap<u32, ProcState>,
}

impl StateMap {
    pub fn new() -> StateMap {
        StateMap { procs: HashMap::new() }
    }

    pub fn add_proc(&mut self, pid: u32) {
        self.procs.entry(pid).or_insert_with(|| ProcState {
            pid,
            regions: Vec::new(),
            rss_kb: 0,
            swap_kb: 0,
        });
    }

    pub fn remove_proc(&mut self, pid: u32) {
        self.procs.remove(&pid);
    }

    /// maps 刷新:新区域继承同 va 的旧年龄/冷却;旧有区域原位保留。
    pub fn refresh_regions(&mut self, pid: u32, regions: &[(u64, u64)]) {
        let p = match self.procs.get_mut(&pid) {
            Some(p) => p,
            None => return,
        };
        let old: Vec<RegionState> = std::mem::take(&mut p.regions);
        p.regions = regions
            .iter()
            .map(|&(s, e)| {
                // 若与旧区域完全同界,原样保留(年龄/位图)
                if let Some(o) = old.iter().find(|o| o.start == s && o.end == e) {
                    return o.clone();
                }
                let mut r = RegionState::new(s, e);
                // va 重叠部分继承年龄与冷却(区域可能扩展/收缩)
                if let Some(o) = old.iter().find(|o| o.start == s) {
                    for i in 0..r.npages {
                        let va = r.start + (i as u64) * 4096;
                        if va >= o.start && va < o.end {
                            let oi = ((va - o.start) / 4096) as usize;
                            r.age[i] = o.age.get(oi).copied().unwrap_or(0);
                            r.last_swapin[i] = o.last_swapin.get(oi).copied().unwrap_or(0);
                        }
                    }
                }
                r
            })
            .collect();
    }

    /// 分类器访问落地:swap-in 访问更新冷却时间
    pub fn note_access(&mut self, pid: u32, va: u64, kind: AccessKind, epoch_s: u32) {
        let _ = kind;
        if let Some(p) = self.procs.get_mut(&pid) {
            if let Some((ri, i)) = p.va_index(va) {
                let r = &mut p.regions[ri];
                if i < r.last_swapin.len() {
                    r.last_swapin[i] = epoch_s;
                }
            }
        }
    }

}
