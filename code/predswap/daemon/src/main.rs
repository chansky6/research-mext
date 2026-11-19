// predswapd 主程序(P1:事件流 + 分类 + record/replay + page_idle 扫描 + 卸载控制器 + ctl)
// 用法:
//   predswapd run --cgroup <v1 memory cgroup 路径> --budget-mb N [--headroom-mb N]
//                 [--record FILE] [--scan-period S] [--cooldown S] [--conf FILE] ...
//   predswapd replay <trace>
//   predswapd status
mod config;
mod ctl;
mod event;
mod offload;
mod pageidle;
mod procinfo;
mod record;
mod state;

use event::{AccessKind, Classifier, EvKind, SwapEv, EV_SZ};
use offload::{OffloadConfig, OffloadStats};
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
        let key = a.trim_start_matches("--").replace('-', "_");
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

fn main() {
    let args: Vec<String> = std::env::args().skip(1).collect();
    let cmd = args.first().map(|s| s.as_str()).unwrap_or("run");
    let rest: Vec<String> = args.iter().skip(1).cloned().collect();
    match cmd {
        "replay" => cmd_replay(&rest),
        "status" => print!("{}", ctl::client_status()),
        "run" => cmd_run(&rest),
        _ => {
            eprintln!("usage: predswapd run|replay|status ...");
            std::process::exit(2);
        }
    }
}

fn cmd_run(args: &[String]) {
    let (kv, conf, _) = parse_cli_kv(args);
    let cfg = config::load(&kv, conf.as_deref());
    if cfg.cgroup.is_empty() || cfg.budget_bytes == 0 {
        eprintln!("run: 需要 --cgroup 与 --budget-mb");
        std::process::exit(2);
    }
    set_memlock_unlimited();
    std::fs::create_dir_all("/tmp/predswap").ok();

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

    // ---- ingest + 分类线程 ----
    {
        let state = state.clone();
        let cnt = cnt.clone();
        let rec = recorder.clone();
        let stop = stop.clone();
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
                cls.feed(&ev);
                if let Some(r) = rec.lock().unwrap().as_mut() {
                    let _ = r.ev(&buf);
                }
                if ev.ts_ns > last_poll_ns {
                    last_poll_ns = ev.ts_ns;
                    cls.poll(ev.ts_ns);
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
                let stats = offload::tick(&oc, &state, majors, epoch, |tgid, segs, ret| {
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
        std::thread::spawn(move || {
            loop {
                ctl::serve_one(&ctl_l, || {
                    status_text(&state, &cnt, &offl, &cfg, &start)
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
}

static STOP_PTR: AtomicU64 = AtomicU64::new(0);
#[allow(clippy::upper_case_acronyms)]

extern "C" fn handle_term(_sig: libc::c_int) {
    let p = STOP_PTR.load(Ordering::SeqCst) as *const AtomicBool;
    if !p.is_null() {
        unsafe { (*p).store(true, Ordering::Relaxed) };
    }
}

fn status_text(
    state: &Arc<Mutex<StateMap>>,
    cnt: &Counters,
    offl: &Arc<Mutex<OffloadStats>>,
    cfg: &config::Config,
    start: &Instant,
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

fn cmd_replay(args: &[String]) {
    let Some(path) = args.first() else {
        eprintln!("replay: 需要 trace 路径");
        std::process::exit(2);
    };
    let items = match record::read_all(std::path::Path::new(path)) {
        Ok(v) => v,
        Err(e) => {
            eprintln!("replay: {e}");
            std::process::exit(1);
        }
    };
    // 1) 用 'E' 重放分类
    let mut cls = Classifier::new();
    let mut n_ev = 0;
    let mut last_ts = 0u64;
    let mut replay: Vec<event::Access> = Vec::new();
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
    replay.extend(cls.drain());

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
