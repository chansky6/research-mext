// 卸载控制器(规格 §6,= 基线 B2)。周期 1s:
//   usage = cgroup memory.usage_in_bytes;目标 B - H。
//   超目标 → ArtMem 式直方图容量反推阈值(从最老桶累加),选中冷页按 va 合并成段,
//   process_madvise(MADV_PAGEOUT) 跨进程换出(Phase 0 §2-1 验证)。
//   物理释放由 cgroup 限额对 swap cache 的逐出完成(Phase 0 §2-5)。
//   护栏(无 PSI,v1):major 斜率超阈值即暂停主动换出。
use crate::state::StateMap;
use std::fs;
use std::sync::Mutex;

const SYS_PIDFD_OPEN: libc::c_long = 434;
const SYS_PROCESS_MADVISE: libc::c_long = 440;
const MADV_PAGEOUT: libc::c_int = 21;
const MAX_IOV: usize = 1024;

pub struct OffloadConfig {
    pub cgroup: String,
    pub scan_period_s: u64,
    pub budget_bytes: u64,
    pub headroom_bytes: u64,
    pub cooldown_s: u32,
    pub max_pageout_bytes: u64,
    /// major/s 超过此值暂停(护栏,规格 §6)
    pub major_guard_ps: u64,
}

#[derive(Default)]
pub struct OffloadStats {
    pub last_usage: u64,
    pub last_pageout_bytes: u64,
    pub threshold_age: Option<u32>,
    pub paused_guard: bool,
    pub candidates: u64,
}

fn usage_in_bytes(cgroup: &str) -> u64 {
    fs::read_to_string(format!("{cgroup}/memory.usage_in_bytes"))
        .ok()
        .and_then(|s| s.trim().parse().ok())
        .unwrap_or(0)
}

fn pidfd_open(pid: u32) -> i32 {
    unsafe { libc::syscall(SYS_PIDFD_OPEN, pid as libc::c_int, 0u32) as i32 }
}

/// 跨进程 process_madvise(MADV_PAGEOUT)。返回建议字节数(Phase 0 §2-1 语义)。
/// pidfd 每批开/用后关:1s 一轮,开销可忽略,无陈旧 fd。
pub fn pageout(pid: u32, segs: &[(u64, u64)]) -> i64 {
    if segs.is_empty() {
        return 0;
    }
    let fd = pidfd_open(pid);
    if fd < 0 {
        return -1;
    }
    let mut total: i64 = 0;
    for chunk in segs.chunks(MAX_IOV) {
        let iov: Vec<libc::iovec> = chunk
            .iter()
            .map(|&(a, l)| libc::iovec {
                iov_base: a as *mut libc::c_void,
                iov_len: l as usize,
            })
            .collect();
        total += unsafe {
            libc::syscall(
                SYS_PROCESS_MADVISE,
                fd,
                iov.as_ptr(),
                iov.len() as libc::c_ulong,
                MADV_PAGEOUT,
                0u32,
            ) as i64
        };
        if total < 0 {
            break;
        }
    }
    unsafe { libc::close(fd) };
    total
}

/// 一轮控制器决策与执行。
pub fn tick(
    cfg: &OffloadConfig,
    state: &Mutex<StateMap>,
    majors_last_sec: u64,
    now_epoch: u32,
    // 段执行后调用:(tgid, segs) 供录制/状态
    mut on_pageout: impl FnMut(u32, &[(u64, u64)], i64),
) -> OffloadStats {
    let usage = usage_in_bytes(&cfg.cgroup);
    let target = cfg.budget_bytes.saturating_sub(cfg.headroom_bytes);
    let mut stats = OffloadStats {
        last_usage: usage,
        paused_guard: majors_last_sec > cfg.major_guard_ps,
        ..Default::default()
    };
    if usage <= target || stats.paused_guard {
        return stats;
    }
    let deficit = usage - target;

    // 1) 收集候选:(pid, 区域, 页, age);同时按 age 累计直方图
    let mut st = state.lock().unwrap();
    struct Cand {
        pid: u32,
        ri: usize,
        i: usize,
        age: u32,
    }
    let mut cands: Vec<Cand> = Vec::new();
    let mut hist: std::collections::BTreeMap<u32, u64> = std::collections::BTreeMap::new();
    for (pid, p) in st.procs.iter_mut() {
        for (ri, r) in p.regions.iter_mut().enumerate() {
            for i in 0..r.npages {
                if r.is_swapped(i) || r.is_offl_pending(i) {
                    continue;
                }
                // 冷却期:最近 swap-in 回到 DRAM 的页不许立刻再换出(规格 §6)
                let ls = r.last_swapin.get(i).copied().unwrap_or(0);
                if ls != 0 && now_epoch.saturating_sub(ls) < cfg.cooldown_s {
                    continue;
                }
                let age = r.age[i];
                if age == 0 {
                    continue; // 上一扫描周期刚访问过
                }
                *hist.entry(age).or_insert(0) += 1;
                cands.push(Cand { pid: *pid, ri, i, age });
            }
        }
    }
    stats.candidates = cands.len() as u64;
    if hist.is_empty() {
        return stats;
    }

    // 2) 容量反推阈值:从最老往新累加,覆盖 deficit(×4KB)即止(ArtMem §5)
    let mut acc: u64 = 0;
    let mut thr: u32 = *hist.keys().next().unwrap();
    for (&a, &n) in hist.iter().rev() {
        acc += n * 4096;
        thr = a;
        if acc >= deficit {
            break;
        }
    }
    // 最小可选页龄 15s:MEXT 口径最细档;scan 周期粗于 15s 时至少 1 epoch
    let min_epochs = 15u64.div_ceil(cfg.scan_period_s.max(1)) as u32;
    let thr = thr.max(min_epochs);
    stats.threshold_age = Some(thr);

    // 3) 选中 + 按 va 排序 + 相邻合并成段
    cands.sort_by_key(|c| (c.pid, c.ri, c.i));
    let mut bytes = 0u64;
    let page_limit = deficit.div_ceil(4096).min(cfg.max_pageout_bytes / 4096);
    let mut by_proc: std::collections::BTreeMap<u32, Vec<(u64, u64)>> =
        std::collections::BTreeMap::new();
    let mut idx = 0usize;
    while idx < cands.len() && bytes / 4096 < page_limit {
        if cands[idx].age < thr {
            idx += 1;
            continue;
        }
        // 段起点:首个达标候选
        let (pid, ri, i0) = (cands[idx].pid, cands[idx].ri, cands[idx].i);
        let mut i1 = i0;
        let start = idx;
        let remaining = page_limit - bytes / 4096;
        loop {
            if idx >= cands.len() || (idx - start) as u64 >= remaining {
                break;
            }
            let n = &cands[idx];
            if n.pid != pid || n.ri != ri || n.age < thr {
                break;
            }
            // n.i == i1:首元素(i1==i0)或延续?首元素直接吃掉;之后要求 i1+1
            if n.i != i0 && n.i != i1 + 1 {
                break;
            }
            i1 = n.i;
            idx += 1;
        }
        let npages = (i1 - i0 + 1) as u64;
        let seg = (pid_base(&st, pid, ri) + (i0 as u64) * 4096, npages * 4096);
        bytes += seg.1;
        // 标记 offl_pending,防止下轮重复选中(pagemap 对账后自然清除)
        if let Some(p) = st.procs.get_mut(&pid) {
            if let Some(r) = p.regions.get_mut(ri) {
                for i in i0..=i1 {
                    let w = i / 64;
                    if w < r.offl_pending.len() {
                        r.offl_pending[w] |= 1 << (i % 64);
                    }
                }
            }
        }
        by_proc.entry(pid).or_default().push(seg);
    }

    // 4) 执行(锁外空间:st 在上面块尾已释放?——不会,同一作用域;pageout 不触碰 state)
    drop(st);
    for (pid, segs) in by_proc {
        let r = pageout(pid, &segs);
        stats.last_pageout_bytes += r.max(0) as u64;
        on_pageout(pid, &segs, r);
    }
    stats
}

fn pid_base(st: &StateMap, pid: u32, ri: usize) -> u64 {
    st.procs
        .get(&pid)
        .and_then(|p| p.regions.get(ri))
        .map(|r| r.start)
        .unwrap_or(0)
}
