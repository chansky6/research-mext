// predswapd 主程序(P1:事件流 + 分类 + record/replay + page_idle 扫描 + 卸载控制器 + ctl)
// 用法:
//   predswapd run --cgroup <v1 memory cgroup 路径> --budget-mb N [--headroom-mb N]
//                 [--record FILE] [--scan-period S] [--cooldown S] [--conf FILE] ...
//   predswapd replay <trace> [--m4-mode trained|m2-only|off] (run 同样支持,默认 trained)
//   predswapd status
mod config;
mod ctl;
mod event;
mod agentm;
mod m4;
mod tinyseq;
mod offload;
mod predictors;
mod pageidle;
mod procinfo;
mod record;
mod state;

use event::{AccessKind, Classifier, EvKind, SwapEv, EV_SZ};
use agentm::{AgentM, IoBudget};
use offload::{OffloadConfig, OffloadStats};
use predictors::{NAMES, N_MEMBERS};
use record::{RecItem, Recorder};
use state::StateMap;
use std::io::{Read, Write};
use std::process::{Command, Stdio};
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

struct Counters {
    ev: AtomicU64,
    swapin: AtomicU64,
    miss: AtomicU64,
    hit: AtomicU64,
    major: AtomicU64,
    late: AtomicU64,
    major_rate: AtomicU64, // 最近 1s
}

fn now_mono_ns() -> u64 {
    let mut ts = libc::timespec { tv_sec: 0, tv_nsec: 0 };
    unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    (ts.tv_sec as u64) * 1_000_000_000 + ts.tv_nsec as u64
}

fn set_memlock_unlimited() {
    let r = libc::rlimit {
        rlim_cur: libc::RLIM_INFINITY,
        rlim_max: libc::RLIM_INFINITY,
    };
    unsafe { libc::setrlimit(libc::RLIMIT_MEMLOCK, &r) };
}

fn parse_cli_kv(args: &[String]) -> (Vec<(String, String)>, Option<String>, Option<String>) {
    // 返回 (键值对, conf 路径, 位置参数)
    let mut kv = Vec::new();
    let mut conf = None;
    let mut pos = None;
    let mut i = 0;
    while i < args.len() {
        let a = &args[i];
        let key = match a.as_str() {
            "--scan-period" => "scan_period_s".to_owned(),
            "--cooldown" => "cooldown_s".to_owned(),
            _ => a.trim_start_matches("--").replace('-', "_"),
        };
        if a == "--conf" {
            conf = args.get(i + 1).cloned();
            i += 2;
        } else if a.starts_with("--") && i + 1 < args.len() {
            kv.push((key, args[i + 1].clone()));
            i += 2;
        } else {
            pos = Some(a.clone());
            i += 1;
        }
    }
    (kv, conf, pos)
}

const M4_USAGE: &str = "--m4-mode trained|m2-only|off (default: trained)";

fn make_m4(mode: &str, replay: bool) -> Option<m4::M4> {
    match mode {
        "trained" if replay => Some(m4::M4::replay()),
        "trained" => Some(m4::M4::new()),
        "m2-only" if replay => Some(m4::M4::m2_only_replay()),
        "m2-only" => Some(m4::M4::m2_only()),
        "off" => None,
        _ => {
            eprintln!("invalid M4 mode {mode:?}; expected {M4_USAGE}");
            std::process::exit(2);
        }
    }
}

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let cmd = args.first().map(|s| s.as_str()).unwrap_or("run");
    let rest: Vec<String> = args.iter().skip(1).cloned().collect();
    match cmd {
        "replay" => cmd_replay(&rest),
        "status" => print!("{}", ctl::client_status()),
        "run" => cmd_run(&rest),
        _ => {
            eprintln!("usage: predswapd run ... [{M4_USAGE}] | replay TRACE [{M4_USAGE}] | status");
            std::process::exit(2);
        }
    }
}

fn cmd_run(args: &[String]) {
    let (kv, conf, _) = parse_cli_kv(args);
    let cfg = config::load(&kv, conf.as_deref());
    let mode = kv.iter().rev().find(|(k, _)| k == "m4_mode").map_or("trained", |(_, v)| v.as_str());
    if args.last().is_some_and(|a| a == "--m4-mode") {
        eprintln!("run: expected {M4_USAGE}");
        std::process::exit(2);
    }
    if cfg.cgroup.is_empty() || cfg.budget_bytes == 0 {
        eprintln!("run: 需要 --cgroup 与 --budget-mb");
        std::process::exit(2);
    }
    let offload_only = std::env::var_os("PREDSWAP_OFFLOAD_ONLY").as_deref() == Some(std::ffi::OsStr::new("1"));
    let m4 = make_m4(mode, false);
    set_memlock_unlimited();
    std::fs::create_dir_all("/tmp/predswap").ok();
    // Normally complement native readahead; replace it only for attribution runs.
    let replace_ra = !offload_only && std::env::var_os("PREDSWAP_REPLACE_RA").as_deref() == Some(std::ffi::OsStr::new("1"));
    let ra_state = if replace_ra { disable_kernel_swap_readahead() } else { (String::new(), String::new()) };

    let start = Instant::now();
    let mut child = Command::new(&cfg.bridge_path)
        .arg(&cfg.bpf_obj)
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .spawn()
        .unwrap_or_else(|e| {
            eprintln!("run: 无法启动 bridge({}): {e}", cfg.bridge_path);
            std::process::exit(1);
        });
    let bridge_in: Arc<Mutex<Option<std::process::ChildStdin>>> =
        Arc::new(Mutex::new(child.stdin.take()));
    let mut bridge_out = child.stdout.take().unwrap();

    let state = Arc::new(Mutex::new(StateMap::new()));
    let cnt = Arc::new(Counters {
        ev: AtomicU64::new(0),
        swapin: AtomicU64::new(0),
        miss: AtomicU64::new(0),
        hit: AtomicU64::new(0),
        major: AtomicU64::new(0),
        late: AtomicU64::new(0),
        major_rate: AtomicU64::new(0),
    });
    let offl_stats: Arc<Mutex<OffloadStats>> = Arc::new(Mutex::new(OffloadStats::default()));
    let recorder: Arc<Mutex<Option<Recorder>>> = Arc::new(Mutex::new(
        cfg.record_path.as_deref().map(|p| Recorder::open(std::path::Path::new(p)).ok()).flatten(),
    ));
    let stop = Arc::new(AtomicBool::new(false));
    STOP_PTR.store(Arc::as_ptr(&stop) as u64, Ordering::SeqCst);
    let watched: Arc<Mutex<Vec<u32>>> = Arc::new(Mutex::new(Vec::new()));
    let preds = Arc::new(Mutex::new(predictors::Predictors::new(
        cfg.lookahead,
        (cfg.window_s * 1e9) as u64,
        cfg.lambda,
        cfg.eta,
        cfg.m2_cap,
        m4,
    )));
    if let Ok(name) = std::env::var("PREDSWAP_ONLY_MEMBER") {
        preds.lock().unwrap().only_member = Some(NAMES.iter().position(|&m| m == name)
            .expect("PREDSWAP_ONLY_MEMBER must be M0, M1, M2, M3, or M4"));
    }
    let agentm = Arc::new(Mutex::new(match AgentM::new() {
        Ok(a) => a,
        Err(e) => {
            eprintln!("run: agent socket 失败: {e}(预取将不可用,卸载照常)");
            return;
        }
    }));
    let iob = Arc::new(Mutex::new(IoBudget::new(cfg.device_iops, cfg.io_share, 175_000)));

    // ---- ingest + 分类线程 ----
    {
        let state = state.clone();
        let cnt = cnt.clone();
        let rec = recorder.clone();
        let stop = stop.clone();
        let preds = preds.clone();
        let agentm = agentm.clone();
        let iob = iob.clone();
        let shadow_only = std::env::var_os("PREDSWAP_SHADOW_ONLY").as_deref() == Some(std::ffi::OsStr::new("1"));
        std::thread::spawn(move || {
            let mut cls = Classifier::new();
            let mut buf = [0u8; EV_SZ];
            let mut last_poll_ns = 0u64;
            loop {
                if stop.load(Ordering::Relaxed) {
                    break;
                }
                let mut n = 0;
                while n < EV_SZ {
                    match bridge_out.read(&mut buf[n..]) {
                        Ok(0) => {
                            stop.store(true, Ordering::Relaxed); // bridge 退出
                            return;
                        }
                        Ok(k) => n += k,
                        Err(e) => {
                            eprintln!("ingest: read err {e}");
                            stop.store(true, Ordering::Relaxed);
                            return;
                        }
                    }
                }
                let ev = SwapEv::parse(&buf);
                cnt.ev.fetch_add(1, Ordering::Relaxed);
                match ev.kind {
                    EvKind::Swapin => cnt.swapin.fetch_add(1, Ordering::Relaxed),
                    EvKind::Miss => cnt.miss.fetch_add(1, Ordering::Relaxed),
                };
                let first_fault = cls.feed(&ev);
                if let Some(r) = rec.lock().unwrap().as_mut() {
                    let _ = r.ev(&buf);
                }
                if ev.ts_ns > last_poll_ns {
                    last_poll_ns = ev.ts_ns;
                    cls.poll(ev.ts_ns);
                }
                if first_fault && !offload_only {
                    let a = event::Fault::from(&ev);
                    let mut p = preds.lock().unwrap();
                    p.ledger.fault_open(&a);
                    let cands = p.propose(&a, now_mono_ns());
                    let issue = !shadow_only && !p.dispatch_off && !cands.is_empty();
                    drop(p);
                    if issue { dispatch_batch(&a,&cands,&state,&preds,&agentm,&iob,&rec,now_mono_ns()); }
                }
                for a in cls.drain() {
                    match a.kind {
                        AccessKind::Hit => { cnt.hit.fetch_add(1, Ordering::Relaxed); }
                        AccessKind::Major => { cnt.major.fetch_add(1, Ordering::Relaxed); }
                        AccessKind::Late => { cnt.late.fetch_add(1, Ordering::Relaxed); }
                    }
                    let epoch = (now_mono_ns() / 1_000_000_000) as u32;
                    state.lock().unwrap().note_access(a.tgid, a.va, a.kind, epoch);
                    if let Some(r) = rec.lock().unwrap().as_mut() {
                        let _ = r.access(&a);
                    }
                    if !offload_only { preds.lock().unwrap().ledger.fault_closed(&a); }
                }
            }
        });
    }

    // ---- 扫描线程:maps 刷新 + pagemap 对账 + page_idle 年龄 + tgid 发现 ----
    {
        let state = state.clone();
        let cfg = cfg.clone();
        let cnt = cnt.clone();
        let stop = stop.clone();
        let watched = watched.clone();
        let bridge_in = bridge_in.clone();
        std::thread::spawn(move || {
            let mut idle = match pageidle::IdlePage::new() {
                Ok(v) => v,
                Err(e) => {
                    eprintln!("scan: page_idle 不可用: {e}");
                    stop.store(true, Ordering::Relaxed);
                    return;
                }
            };
            let mut prev_major = 0u64;
            loop {
                if stop.load(Ordering::Relaxed) {
                    break;
                }
                // tgid 发现(cgroup tasks → tgid 集合;diff → 通知 bridge + state)
                match procinfo::cgroup_tgids(&cfg.cgroup) {
                    Err(e) => eprintln!("scan: cgroup_tgids({}): {e}", cfg.cgroup),
                    Ok(tgids) => {
                    eprintln!("scan: discovered {} tgids: {:?}", tgids.len(), &tgids);
                    let mut w = watched.lock().unwrap();
                    for &t in &tgids {
                        if !w.contains(&t) {
                            w.push(t);
                            if let Some(bi) = bridge_in.lock().unwrap().as_mut() {
                                let _ = writeln!(bi, "+{t}");
                            }
                            state.lock().unwrap().add_proc(t);
                        }
                    }
                    w.retain(|&t| {
                        if tgids.contains(&t) {
                            true
                        } else {
                            if let Some(bi) = bridge_in.lock().unwrap().as_mut() {
                                let _ = writeln!(bi, "-{t}");
                            }
                            state.lock().unwrap().remove_proc(t);
                            false
                        }
                    });
                    }
                }
                // 每 scan_period:区域刷新 + pagemap + idle 年龄
                let procs: Vec<u32> = watched.lock().unwrap().clone();
                let mut pfn_map: std::collections::HashMap<(u32, usize, usize), u32> =
                    std::collections::HashMap::new();
                for &pid in &procs {
                    let regions = match procinfo::anon_regions(pid) {
                        Ok(r) => r,
                        Err(_) => continue,
                    };
                    let meta: Vec<(u64, u64)> = regions.iter().map(|r| (r.start, r.end)).collect();
                    state.lock().unwrap().refresh_regions(pid, &meta);
                    if let Ok(scans) = procinfo::pagemap_scan(pid, &regions) {
                        let mut st = state.lock().unwrap();
                        if let Some(p) = st.procs.get_mut(&pid) {
                            for (ri, sc) in scans.iter().enumerate() {
                                let r = &mut p.regions[ri];
                                r.resident.clear();
                                for i in 0..sc.class.len() {
                                    match sc.class[i] {
                                        1 => {
                                            r.swapped[i / 64] &= !(1 << (i % 64));
                                            r.offl_pending[i / 64] &= !(1 << (i % 64));
                                            r.resident.push(i as u32);
                                            if let Some(pf) = sc.pfn[i] {
                                                pfn_map.insert((pid, ri, i), pf);
                                            }
                                        }
                                        2 => r.swapped[i / 64] |= 1 << (i % 64),
                                        _ => {}
                                    }
                                }
                            }
                        }
                    }
                }
                let pfns: Vec<u32> = pfn_map.values().copied().collect();
                eprintln!("scan: resident pfn={} (armed prev={})", pfns.len(), idle.armed_len());
                if let Ok(res) = idle.scan(&pfns) {
                    let n_idle = res.iter().filter(|r| r.1).count();
                    eprintln!("scan: idle.scan -> {} resident, {} idle", res.len(), n_idle);
                    // res: (pfn, idle) → 回写 age
                    let mut by_pfn: std::collections::HashMap<u32, bool> =
                        res.into_iter().collect();
                    let mut st = state.lock().unwrap();
                    for ((pid, ri, i), pf) in pfn_map.iter() {
                        if let Some(idle_flag) = by_pfn.get_mut(pf) {
                            if let Some(p) = st.procs.get_mut(pid) {
                                if let Some(r) = p.regions.get_mut(*ri) {
                                    if *idle_flag {
                                        r.age[*i] = r.age[*i].saturating_add(1);
                                    } else {
                                        r.age[*i] = 0;
                                    }
                                }
                            }
                        }
                    }
                }
                // rss/swap 刷新
                {
                    let mut st = state.lock().unwrap();
                    for &pid in &procs {
                        let (rss, swp) = procinfo::rss_swap_kb(pid);
                        if let Some(p) = st.procs.get_mut(&pid) {
                            p.rss_kb = rss;
                            p.swap_kb = swp;
                        }
                    }
                }
                // major 速率(护栏输入)
                let m = cnt.major.load(Ordering::Relaxed);
                cnt.major_rate
                    .store(m.saturating_sub(prev_major) / cfg.scan_period_s.max(1), Ordering::Relaxed);
                prev_major = m;

                std::thread::sleep(Duration::from_secs(cfg.scan_period_s));
            }
        });
    }

    // ---- 控制器线程 ----
    {
        let state = state.clone();
        let cnt = cnt.clone();
        let cfg = cfg.clone();
        let rec = recorder.clone();
        let stop = stop.clone();
        let offl = offl_stats.clone();
        let preds = preds.clone();
        let agentm = agentm.clone();
        let iob = iob.clone();
        let watched = watched.clone();
        let ra_pc = ra_state.0.clone();
        let ra_vma = ra_state.1.clone();
        let mut prev_accesses = 0u64;
        let mut off_epochs = 0u32;
        let mut on_epochs = 0u32;
        let mut ra_currently_off = replace_ra;
        std::thread::spawn(move || {
            loop {
                if stop.load(Ordering::Relaxed) {
                    break;
                }
                let oc = OffloadConfig {
                    cgroup: cfg.cgroup.clone(),
                    scan_period_s: cfg.scan_period_s,
                    budget_bytes: cfg.budget_bytes,
                    headroom_bytes: if cfg.headroom_bytes != 0 {
                        cfg.headroom_bytes
                    } else {
                        (2 * cfg.budget_bytes / 100).max(64 << 20)
                    },
                    cooldown_s: cfg.cooldown_s,
                    max_pageout_bytes: cfg.max_pageout_bytes,
                    major_guard_ps: cfg.major_guard_ps,
                };
                let epoch = (now_mono_ns() / 1_000_000_000) as u32;
                let majors = cnt.major_rate.load(Ordering::Relaxed);
                let ts = now_mono_ns();
                // agent 消息(HELLO/ACK)+ 目录兜底
                {
                    let mut am = agentm.lock().unwrap();
                    am.poll_msgs();
                    let w = watched.lock().unwrap().clone();
                    am.scan_dir(&w);
                }
                // P2:选择器 epoch tick + IO 令牌 AIMD
                let accesses_now = cnt.hit.load(Ordering::Relaxed)
                    + cnt.major.load(Ordering::Relaxed)
                    + cnt.late.load(Ordering::Relaxed);
                let accesses = accesses_now.saturating_sub(prev_accesses);
                prev_accesses = accesses_now;
                {
                    let p90 = preds.lock().unwrap().ledger.major_p90_ns;
                    iob.lock().unwrap().set_window_major_p90(p90);
                    let rate = iob.lock().unwrap().rate_per_s();
                    iob.lock().unwrap().tick();
                    preds.lock().unwrap().epoch_tick(ts, accesses, rate);
                }
                // 内核 swap 预读的开关(M1 覆盖它;M0 接管则还给内核),2 epoch 迟滞
                if replace_ra {
                    let off = preds.lock().unwrap().dispatch_off;
                    if off {
                        off_epochs = off_epochs.saturating_add(1);
                        on_epochs = 0;
                    } else {
                        on_epochs = on_epochs.saturating_add(1);
                        off_epochs = 0;
                    }
                    if off_epochs >= 2 && ra_currently_off {
                        let _ = std::fs::write("/proc/sys/vm/page-cluster", &ra_pc);
                        let _ = std::fs::write("/sys/kernel/mm/swap/vma_ra_enabled", &ra_vma);
                        ra_currently_off = false;
                    } else if on_epochs >= 2 && !ra_currently_off {
                        let _ = std::fs::write("/proc/sys/vm/page-cluster", "0");
                        let _ = std::fs::write("/sys/kernel/mm/swap/vma_ra_enabled", "false");
                        ra_currently_off = true;
                    }
                }
                let stats = offload::tick(&oc, &state, majors, epoch, |tgid, segs, ret| {
                    if ret > 0 && !offload_only {
                        preds.lock().unwrap().register_group(tgid, segs);
                    }
                    if let Some(r) = rec.lock().unwrap().as_mut() {
                        let _ = r.pageout(ts, tgid, segs, ret);
                    }
                });
                if stats.last_pageout_bytes > 0 || stats.paused_guard {
                    eprintln!("ctrl: usage={}MB pageout={}MB thr={:?} paused={}",
                        stats.last_usage >> 20, stats.last_pageout_bytes >> 20, stats.threshold_age, stats.paused_guard);
                }
                *offl.lock().unwrap() = stats;
                std::thread::sleep(Duration::from_secs(1));
            }
        });
    }

    // ---- ctl 线程 ----
    let ctl_l = match ctl::start_server() {
        Ok(l) => l,
        Err(e) => {
            eprintln!("ctl socket 失败: {e}");
            std::process::exit(1);
        }
    };
    {
        let state = state.clone();
        let cnt = cnt.clone();
        let offl = offl_stats.clone();
        let cfg = cfg.clone();
        let start = start.clone();
        let preds = preds.clone();
        let agentm = agentm.clone();
        let iob = iob.clone();
        std::thread::spawn(move || {
            loop {
                ctl::serve_one(&ctl_l, || {
                    status_text(&state, &cnt, &offl, &cfg, &start, &preds, &agentm, &iob)
                });
            }
        });
    }

    // ---- 主循环:等 SIGTERM/SIGINT(轮询近似,P1 够用;kill -9 由 EPIPE 链兜底) ----
    unsafe {
        libc::signal(libc::SIGINT, handle_term as *const () as usize);
        libc::signal(libc::SIGTERM, handle_term as *const () as usize);
    }
    while !stop.load(Ordering::Relaxed) {
        std::thread::sleep(Duration::from_millis(200));
    }
    if let Some(mut bi) = bridge_in.lock().unwrap().take() {
        let _ = writeln!(bi, "quit");
        let _ = bi.flush();
    }
    let _ = child.kill();
    let _ = child.wait();
    {
        let mut g = recorder.lock().unwrap();
        if let Some(r) = g.as_mut() {
            let _ = r.flush();
        }
    }
    restore_kernel_swap_readahead(ra_state);
}

/// (page-cluster 原值, vma_ra 原值);读取失败按已关处理
fn disable_kernel_swap_readahead() -> (String, String) {
    let pc = std::fs::read_to_string("/proc/sys/vm/page-cluster").unwrap_or_default();
    let vma = std::fs::read_to_string("/sys/kernel/mm/swap/vma_ra_enabled").unwrap_or_default();
    let _ = std::fs::write("/proc/sys/vm/page-cluster", "0");
    let _ = std::fs::write("/sys/kernel/mm/swap/vma_ra_enabled", "false");
    (pc.trim().to_string(), vma.trim().to_string())
}

fn restore_kernel_swap_readahead(state: (String, String)) {
    let (pc, vma) = state;
    if !pc.is_empty() {
        let _ = std::fs::write("/proc/sys/vm/page-cluster", &pc);
    }
    if !vma.is_empty() {
        let _ = std::fs::write("/sys/kernel/mm/swap/vma_ra_enabled", &vma);
    }
}

/// 派发管线(规格 §5.4):配额过滤 → 换出态过滤 + 在途去重 → 相邻合并 →
/// IO 令牌 → agent WILLNEED → 账本标记 + 'D' 记录。锁序:state 与 preds 不同时持有。
#[allow(clippy::too_many_arguments)]
fn dispatch_batch(
    a: &event::Fault,
    cands: &[(usize, u64)],
    state: &Arc<Mutex<state::StateMap>>,
    preds: &Arc<Mutex<predictors::Predictors>>,
    agentm: &Arc<Mutex<AgentM>>,
    iob: &Arc<Mutex<IoBudget>>,
    rec: &Arc<Mutex<Option<Recorder>>>,
    now_ns: u64,
) {
    // 3.1 配额内成员的候选页集合(跨成员去重)
    let sel: Vec<u64> = {
        let p = preds.lock().unwrap();
        let mut v: Vec<u64> = cands
            .iter()
            .filter(|(m, _)| (p.member_used[*m] as f64) < p.quota[*m])
            .map(|(_, va)| *va)
            .collect();
        v.sort_unstable();
        v.dedup();
        v
    };
    if sel.is_empty() {
        return;
    }
    // 3.2 state 锁内:查哪些页已换出
    let swapped: Vec<u64> = {
        let st = state.lock().unwrap();
        sel.iter()
            .filter(|&&va| {
                st.procs
                    .get(&a.tgid)
                    .and_then(|pr| pr.va_index(va).map(|(ri, i)| pr.regions[ri].is_swapped(i)))
                    .unwrap_or(false)
            })
            .copied()
            .collect()
    };
    if swapped.is_empty() {
        return;
    }
    // 3.3 preds 锁内:在途去重 + 配额消耗
    let target: Vec<u64> = {
        let p = preds.lock().unwrap();
        swapped
            .into_iter()
            .filter(|&va| !p.ledger.in_flight(a.tgid, va))
            .collect()
    };
    if target.is_empty() {
        return;
    }
    // Charge pages only after enqueue. Lock order matches status_text: agentm then iob.
    let n = {
        let mut am = agentm.lock().unwrap();
        let result = am.dispatch(a.tgid, &target, &mut iob.lock().unwrap());
        let Some((n, segs)) = result else { return };
        if let Some(r) = rec.lock().unwrap().as_mut() {
            let _ = r.dispatch(now_ns, a.tgid, segs);
        }
        n
    };
    let mut p = preds.lock().unwrap();
    for &va in &target[..n] {
        p.ledger.mark_dispatched(a.tgid, va, now_ns);
        for &(m, pva) in cands {
            if pva == va {
                p.member_used[m] += 1;
            }
        }
    }
}

static STOP_PTR: AtomicU64 = AtomicU64::new(0);
#[allow(clippy::upper_case_acronyms)]

extern "C" fn handle_term(_sig: libc::c_int) {
    let p = STOP_PTR.load(Ordering::SeqCst) as *const AtomicBool;
    if !p.is_null() {
        unsafe { (*p).store(true, Ordering::Relaxed) };
    }
}

#[allow(clippy::too_many_arguments)]
fn status_text(
    state: &Arc<Mutex<StateMap>>,
    cnt: &Counters,
    offl: &Arc<Mutex<OffloadStats>>,
    cfg: &config::Config,
    start: &Instant,
    preds: &Arc<Mutex<predictors::Predictors>>,
    agentm: &Arc<Mutex<AgentM>>,
    iob: &Arc<Mutex<IoBudget>>,
) -> String {
    let mut s = String::new();
    s.push_str(&format!(
        "predswapd uptime={:.0}s budget={}MB headroom={}MB\n",
        start.elapsed().as_secs_f64(),
        cfg.budget_bytes >> 20,
        if cfg.headroom_bytes != 0 { cfg.headroom_bytes >> 20 } else { (2 * cfg.budget_bytes / 100).max(64 << 20) >> 20 }
    ));
    s.push_str(&format!(
        "events={} swapin={} miss={} | accesses: hit={} major={} late={} major_rate={}/s\n",
        cnt.ev.load(Ordering::Relaxed),
        cnt.swapin.load(Ordering::Relaxed),
        cnt.miss.load(Ordering::Relaxed),
        cnt.hit.load(Ordering::Relaxed),
        cnt.major.load(Ordering::Relaxed),
        cnt.late.load(Ordering::Relaxed),
        cnt.major_rate.load(Ordering::Relaxed),
    ));
    let o = offl.lock().unwrap();
    s.push_str(&format!(
        "offload: usage={}MB pageout={}MB thr_age_epochs={:?} paused={} candidates={}\n",
        o.last_usage >> 20,
        o.last_pageout_bytes >> 20,
        o.threshold_age,
        o.paused_guard,
        o.candidates,
    ));
    {
        // 锁序:与 state/preds 分开持有
        let p = preds.lock().unwrap();
        let w = p.weights();
        let off = p.dispatch_off;
        s.push_str(&format!(
            "preds: dispatch_off={} w={:.3?} lookahead={}\n",
            off, w, p.lookahead
        ));
        s.push_str(&format!("  M4 runtime: {:?}\n", p.m4_stats()));
        for m in 0..N_MEMBERS {
            let st = &p.ledger.stat[m];
            let acc = if st.preds > 0 { 100.0 * st.hits as f64 / st.preds as f64 } else { 0.0 };
            s.push_str(&format!(
                "  {}: preds={} hits={} shadow_acc={:.1}% addr={} dispatched={} wasted={} quota={} used={}\n",
                NAMES[m], st.preds, st.hits, acc, st.address_matches, st.dispatched, st.wasted_dispatch,
                p.quota[m] as u64, p.member_used[m],
            ));
        }
    }
    {
        let mut am = agentm.lock().unwrap();
        am.poll_msgs(); // Include feedback already queued after the last dispatch/tick.
        let b = iob.lock().unwrap();
        let eff = if am.req_bytes > 0 { 100.0 * am.adv_bytes as f64 / am.req_bytes as f64 } else { 100.0 };
        s.push_str(&format!(
            "agent: pids={} req={}MB adv={}MB eff={:.0}% acks={} errs={} submitted={} queue_full={} peer_gone={} other={}\nio: rate={}/s tokens={:.0} major_p90={}µs\n",
            am.by_pid.len(), am.req_bytes >> 20, am.adv_bytes >> 20, eff, am.acks, am.send_errs,
            am.submitted, am.queue_full, am.peer_gone, am.send_errs - am.queue_full - am.peer_gone,
            b.rate_per_s(), b.tokens, p90_of_preds(preds) / 1000,
        ));
    }
    let st = state.lock().unwrap();
    for p in st.procs.values() {
        let (mut b0, mut b1, mut b2, mut b3, mut b4, mut b5) = (0u64, 0u64, 0u64, 0u64, 0u64, 0u64);
        let mut swapped = 0u64;
        let sp = cfg.scan_period_s.max(1);
        for r in &p.regions {
            for i in 0..r.npages {
                if r.is_swapped(i) {
                    swapped += 1;
                    continue;
                }
                let sec = r.age[i] as u64 * sp;
                // MEXT RAM View 口径:<15s/<1m/<2m/<3m/<5m/>5m(冷端)
                if sec < 15 { b0 += 1 }
                else if sec < 60 { b1 += 1 }
                else if sec < 120 { b2 += 1 }
                else if sec < 180 { b3 += 1 }
                else if sec < 300 { b4 += 1 }
                else { b5 += 1 }
            }
        }
        s.push_str(&format!(
            "  pid={} rss={}MB swap={}MB pages: <15s={} <1m={} <2m={} <3m={} <5m={} >5m={} swapped={}\n",
            p.pid, p.rss_kb / 1024, p.swap_kb / 1024, b0, b1, b2, b3, b4, b5, swapped,
        ));
    }
    s
}

fn p90_of_preds(preds: &Arc<Mutex<predictors::Predictors>>) -> u64 {
    preds.lock().unwrap().ledger.major_p90_ns
}

fn cmd_replay(args: &[String]) {
    let (path, mode) = match args {
        [path] => (path, "trained"),
        [path, flag, mode] if flag == "--m4-mode" => (path, mode.as_str()),
        _ => {
            eprintln!("usage: predswapd replay TRACE [{M4_USAGE}]");
            std::process::exit(2);
        }
    };
    let m4 = make_m4(mode, true);
    let items = match record::read_all(std::path::Path::new(path)) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("replay: {e}");
            std::process::exit(1);
        }
    };
    let mut preds = predictors::Predictors::new(8, 30_000_000_000, 0.5, 0.5, 262144, m4);
    // Only expose pageout groups already known at each historical access.
    let mut groups: Vec<_> = items.iter().filter_map(|it| match it {
        RecItem::Pageout { ts_ns, tgid, ret, segs } if *ret > 0 => Some((*ts_ns, *tgid, segs)),
        _ => None,
    }).collect();
    groups.sort_by_key(|g| g.0);
    let mut groups = groups.into_iter().peekable();

    // 1) 用 'E' 重放分类
    let mut cls = Classifier::new();
    let mut n_ev = 0;
    let mut last_ts = 0u64;
    for it in &items {
        if let RecItem::Ev(e) = it {
            n_ev += 1;
            cls.feed(e);
            if e.ts_ns > last_ts {
                last_ts = e.ts_ns;
                cls.poll(e.ts_ns);
            }
        }
    }
    cls.poll(u64::MAX);
    let mut replay: Vec<event::Access> = cls.drain();
    // Classifier::poll may close multiple pending accesses in hash-table order.
    replay.sort_by_key(|a| (a.ts_ns, a.tgid, a.tid, a.va));
    let mut epoch_accesses = 0u64;
    let mut t_last = replay.first().map_or(0, |a| a.ts_ns);
    for a in &replay {
        while let Some((_, tgid, segs)) = groups.next_if(|g| g.0 <= a.ts_ns) {
            preds.register_group(tgid, segs);
        }
        if a.ts_ns.saturating_sub(t_last) >= 1_000_000_000 {
            preds.epoch_tick(a.ts_ns, epoch_accesses, 7600);
            epoch_accesses = 0;
            t_last = a.ts_ns;
        }
        epoch_accesses += 1;
        let _ = preds.on_access(a, a.ts_ns);
    }
    preds.epoch_tick(replay.last().map_or(0, |a| a.ts_ns), epoch_accesses, 7600);

    // 2) 与录制时写下的 'A' 对照
    let mut rec_hit = 0u64;
    let mut rec_major = 0u64;
    let mut rec_late = 0u64;
    for it in &items {
        if let RecItem::Access(a) = it {
            match a.kind {
                AccessKind::Hit => rec_hit += 1,
                AccessKind::Major => rec_major += 1,
                AccessKind::Late => rec_late += 1,
            }
        }
    }
    let (mut rp_hit, mut rp_major, mut rp_late) = (0u64, 0u64, 0u64);
    for a in &replay {
        match a.kind {
            AccessKind::Hit => rp_hit += 1,
            AccessKind::Major => rp_major += 1,
            AccessKind::Late => rp_late += 1,
        }
    }
    let mut n_po = 0u64;
    let mut po_bytes = 0u64;
    let mut po_ret = 0i64;
    for it in &items {
        if let RecItem::Pageout { tgid: _, ret, segs, .. } = it {
            n_po += 1;
            po_ret += ret;
            for s in segs {
                po_bytes += s.1;
            }
        }
    }
    let total = (rec_hit + rec_major + rec_late).max(1);
    println!(
        "replay {}: events={} accesses(reported/replayed): hit={}/{} major={}/{} late={}/{}",
        path, n_ev, rec_hit, rp_hit, rec_major, rp_major, rec_late, rp_late
    );
    println!(
        "consistency: {}% (差异通常来自 poll 时刻的边界重试对)",
        100 - ((rec_hit + rec_major + rec_late).abs_diff(rp_hit + rp_major + rp_late) * 100) / total
    );
    println!("pageout actions={} ret_bytes={} seg_bytes={}", n_po, po_ret, po_bytes);

    // P2:成员影子指标 + 选择器权重 + 派发记录摘要
    {
        let st = &preds.ledger.stat;
        println!("shadow members (historical replay, M4 mode={mode}; trace-time scheduling, no live dispatch or timing parity):");
        println!("  M4 runtime: {:?}", preds.m4_stats());
        for m in 0..N_MEMBERS {
            let acc = if st[m].preds > 0 { 100.0 * st[m].hits as f64 / st[m].preds as f64 } else { 0.0 };
            println!(
                "  {}: preds={} hits={} shadow_acc={:.1}% address_matches={} dispatched={} wasted_dispatch={}",
                NAMES[m], st[m].preds, st[m].hits, acc, st[m].address_matches, st[m].dispatched, st[m].wasted_dispatch
            );
        }
        let w = preds.weights();
        println!("selector weights: {:.3?} dispatch_off={}", w, preds.dispatch_off);
    }
    {
        let mut n_d = 0u64;
        let mut d_bytes = 0u64;
        for it in &items {
            if let RecItem::Dispatch { bytes, segs, .. } = it {
                n_d += 1;
                d_bytes += *bytes;
                let _ = segs;
            }
        }
        println!("dispatch records: n={} bytes={}MB", n_d, d_bytes >> 20);
    }
    // 重试对间隔分布(major 的 io_wait)
    let mut waits: Vec<u64> = replay
        .iter()
        .filter(|a| a.kind == AccessKind::Major && a.io_wait_ns > 0)
        .map(|a| a.io_wait_ns)
        .collect();
    waits.sort_unstable();
    if !waits.is_empty() {
        println!(
            "major io_wait: n={} p50={}µs p95={}µs",
            waits.len(),
            waits[waits.len() / 2] / 1000,
            waits[(waits.len() * 95) / 100] / 1000
        );
    }
}
