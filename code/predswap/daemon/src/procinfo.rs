// /proc 解析:pagemap 扫描、maps 匿名区发现、cgroup 任务列表(规格 §4.2/§4.3)
use std::collections::HashMap;
use std::fs;
use std::io;
use std::os::unix::fs::FileExt;
use std::path::Path;

pub const PM_PRESENT: u64 = 1 << 63;
pub const PM_SWAP: u64 = 1 << 62;
pub const PM_PFN_MASK: u64 = (1 << 55) - 1;

#[derive(Clone, Debug)]
pub struct AnonRegion {
    pub start: u64,
    pub end: u64,
}

/// 匿名 rw 区(无文件、无 [heap]/[stack] 标注);最小 1MB,避开琐碎映射。
pub fn anon_regions(pid: u32) -> io::Result<Vec<AnonRegion>> {
    let txt = fs::read_to_string(format!("/proc/{pid}/maps"))?;
    let mut out = Vec::new();
    for line in txt.lines() {
        let (rng, rest) = match line.split_once(char::is_whitespace) {
            Some(v) => v,
            None => continue,
        };
        let flags = rest.split_whitespace().next().unwrap_or("");
        if !flags.starts_with("rw") {
            continue;
        }
        // 匿名:rest 里不含路径与 [xxx](只有 offset/dev/inode)
        let last = rest.split_whitespace().last().unwrap_or("");
        if last.contains('/') || last.starts_with('[') {
            continue;
        }
        let (s, e) = match rng.split_once('-') {
            Some(v) => v,
            None => continue,
        };
        let (Ok(s), Ok(e)) = (u64::from_str_radix(s, 16), u64::from_str_radix(e, 16)) else {
            continue;
        };
        if e - s >= 1 << 20 {
            out.push(AnonRegion { start: s, end: e });
        }
    }
    Ok(out)
}

pub struct PageScan {
    /// 区域每页分类:位 0 = present,位 1 = swapped
    pub class: Vec<u8>,
    /// resident 页 PFN(与 class 中 present 位一一对应的位置为 Some)
    pub pfn: Vec<Option<u32>>,
}

/// 一次 pagemap 扫描覆盖 regions 列表;返回与输入同序的每页状态。
pub fn pagemap_scan(pid: u32, regions: &[AnonRegion]) -> io::Result<Vec<PageScan>> {
    let fd = fs::File::open(format!("/proc/{pid}/pagemap"))?;
    let mut out = Vec::with_capacity(regions.len());
    for r in regions {
        let np = ((r.end - r.start) / 4096) as usize;
        let mut raw = vec![0u8; np * 8];
        fd.read_at(&mut raw, r.start / 4096 * 8)?;
        let mut class = vec![0u8; np];
        let mut pfn = vec![Option::<u32>::None; np];
        for i in 0..np {
            let e = u64::from_le_bytes(raw[i * 8..i * 8 + 8].try_into().unwrap());
            if e & PM_PRESENT != 0 {
                class[i] = 1;
                pfn[i] = Some((e & PM_PFN_MASK) as u32);
            } else if e & PM_SWAP != 0 {
                class[i] = 2;
            }
        }
        out.push(PageScan { class, pfn });
    }
    Ok(out)
}

/// cgroup v1 tasks 文件 → tgid 集合(过滤线程 tid:读 TGid)
pub fn cgroup_tgids(cgroup: &str) -> io::Result<Vec<u32>> {
    let tasks = fs::read_to_string(Path::new(cgroup).join("tasks"))?;
    let mut tgids: HashMap<u32, ()> = HashMap::new();
    for l in tasks.lines() {
        let Ok(tid) = l.trim().parse::<u32>() else { continue };
        let tgid = match fs::read_to_string(format!("/proc/{tid}/status")) {
            Ok(s) => s
                .lines()
                .find(|l| l.starts_with("Tgid:"))
                .and_then(|l| l.split_whitespace().nth(1).and_then(|v| v.parse().ok()))
                .unwrap_or(0),
            Err(_) => continue, // 已退出
        };
        if tgid != 0 {
            tgids.entry(tgid).or_insert(());
        }
    }
    Ok(tgids.keys().copied().collect())
}

pub fn rss_swap_kb(pid: u32) -> (u64, u64) {
    let mut rss = 0;
    let mut swp = 0;
    if let Ok(s) = fs::read_to_string(format!("/proc/{pid}/status")) {
        for l in s.lines() {
            if l.starts_with("VmRSS:") {
                rss = l.split_whitespace().nth(1).and_then(|v| v.parse().ok()).unwrap_or(0);
            } else if l.starts_with("VmSwap:") {
                swp = l.split_whitespace().nth(1).and_then(|v| v.parse().ok()).unwrap_or(0);
            }
        }
    }
    (rss, swp)
}
