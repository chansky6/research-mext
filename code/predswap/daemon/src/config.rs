// 配置:CLI 优先于 /etc/predswap.conf(简单 key = value,规格 §7)
use std::collections::HashMap;
use std::fs;

#[derive(Clone)]
pub struct Config {
    pub cgroup: String,
    pub budget_bytes: u64,
    pub headroom_bytes: u64,
    pub scan_period_s: u64,
    pub cooldown_s: u32,
    pub max_pageout_bytes: u64,
    pub major_guard_ps: u64,
    pub bridge_path: String,
    pub bpf_obj: String,
    pub record_path: Option<String>,
}

impl Default for Config {
    fn default() -> Config {
        Config {
            cgroup: String::new(),
            budget_bytes: 0,
            headroom_bytes: 0,
            scan_period_s: 10,
            cooldown_s: 30,
            max_pageout_bytes: 256 << 20,
            major_guard_ps: 500,
            bridge_path: "/opt/predswap/ps_bridge".into(),
            bpf_obj: "/opt/predswap/predswap.bpf.o".into(),
            record_path: None,
        }
    }
}

fn parse_kv(text: &str, out: &mut HashMap<String, String>) {
    for l in text.lines() {
        let l = l.trim();
        if l.is_empty() || l.starts_with('#') {
            continue;
        }
        if let Some((k, v)) = l.split_once('=') {
            out.insert(k.trim().to_string(), v.trim().to_string());
        }
    }
}

fn apply(h: &HashMap<String, String>, c: &mut Config) {
    let get = |k: &str| h.get(k).cloned();
    if let Some(v) = get("cgroup") { c.cgroup = v; }
    if let Some(v) = get("budget_mb") { c.budget_bytes = v.parse().unwrap_or(0) << 20; }
    if let Some(v) = get("headroom_mb") { c.headroom_bytes = v.parse().unwrap_or(0) << 20; }
    if let Some(v) = get("scan_period_s") { c.scan_period_s = v.parse().unwrap_or(10); }
    if let Some(v) = get("cooldown_s") { c.cooldown_s = v.parse().unwrap_or(30); }
    if let Some(v) = get("max_pageout_mb") { c.max_pageout_bytes = v.parse().unwrap_or(256) << 20; }
    if let Some(v) = get("major_guard_ps") { c.major_guard_ps = v.parse().unwrap_or(500); }
    if let Some(v) = get("bridge") { c.bridge_path = v; }
    if let Some(v) = get("bpf") { c.bpf_obj = v; }
    if let Some(v) = get("record") { if !v.is_empty() { c.record_path = Some(v); } }
}

pub fn load(cli_kv: &[(String, String)], conf_path: Option<&str>) -> Config {
    let mut c = Config::default();
    let mut h = HashMap::new();
    if let Some(p) = conf_path {
        if let Ok(t) = fs::read_to_string(p) {
            parse_kv(&t, &mut h);
        }
    }
    for (k, v) in cli_kv {
        h.insert(k.clone(), v.clone());
    }
    apply(&h, &mut c);
    c
}
