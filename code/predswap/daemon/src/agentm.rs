// agent 管理:发现目标进程的 agent socket、派发预取段、收集 ACK(规格 §5.4/§7)。
// 协议见 agent/predswap_agent.c:HELLO/REQ/ACK,unix dgram,/tmp/predswap/。
use std::collections::HashMap;
use std::os::unix::net::UnixDatagram;
use std::path::Path;

const AGENT_DIR: &str = "/tmp/predswap/agents";
const DAEMON_SOCK: &str = "/tmp/predswap/agent.sock";

pub struct AgentM {
    sock: UnixDatagram,
    req_buf: Vec<u8>,
    segments: Vec<(u64, u64)>,
    /// tgid -> agent socket 路径
    pub by_pid: HashMap<u32, String>,
    // Byte counts come from received ACKs only. adv_bytes counts successful
    // madvise lengths, not physical reads, residency, or useful hits.
    pub req_bytes: u64,
    pub adv_bytes: u64,
    pub acks: u64,
    // Failed REQ send_to calls (one per datagram, not per page or madvise).
    pub send_errs: u64,
    // Successful REQ enqueues; submitted - acks includes pending/lost feedback.
    pub submitted: u64,
    pub queue_full: u64,
    pub peer_gone: u64,
}

impl AgentM {
    pub fn new() -> std::io::Result<AgentM> {
        std::fs::create_dir_all(AGENT_DIR)?;
        let _ = std::fs::remove_file(DAEMON_SOCK);
        let sock = UnixDatagram::bind(DAEMON_SOCK)?;
        Self::from_socket(sock)
    }

    fn from_socket(sock: UnixDatagram) -> std::io::Result<Self> {
        sock.set_nonblocking(true)?;
        Ok(Self { sock, req_buf: Vec::new(), segments: Vec::new(), by_pid: HashMap::new(),
            req_bytes: 0, adv_bytes: 0, acks: 0, send_errs: 0,
            submitted: 0, queue_full: 0, peer_gone: 0 })
    }

    /// 非阻塞收 HELLO/ACK(agent 可能先于 daemon 起来,由扫描目录兜底发现)
    pub fn poll_msgs(&mut self) {
        // One extra byte so truncated oversized datagrams cannot look like ACKs.
        let mut buf = [0u8; 33];
        loop {
            match self.sock.recv(&mut buf) {
                Ok(n) => {
                    if n == 8 && buf[..4] == 1u32.to_le_bytes() {
                        let pid = u32::from_le_bytes(buf[4..8].try_into().unwrap());
                        self.by_pid.insert(pid, format!("{AGENT_DIR}/a-{pid}.sock"));
                    } else if n == 32 && buf[..4] == 3u32.to_le_bytes() {
                        let req = u64::from_le_bytes(buf[8..16].try_into().unwrap());
                        let adv = u64::from_le_bytes(buf[16..24].try_into().unwrap());
                        self.req_bytes += req;
                        self.adv_bytes += adv;
                        self.acks += 1;
                    }
                }
                Err(_) => break,
            }
        }
    }

    /// 扫描 agents 目录补登(HELLO 丢失兜底)
    pub fn scan_dir(&mut self, wanted: &[u32]) {
        let _ = std::fs::create_dir_all(AGENT_DIR);
        if let Ok(rd) = std::fs::read_dir(AGENT_DIR) {
            for e in rd.flatten() {
                let name = e.file_name();
                let name = name.to_string_lossy();
                if let Some(pidstr) = name.strip_prefix("a-").and_then(|s| s.strip_suffix(".sock")) {
                    if let Ok(pid) = pidstr.parse::<u32>() {
                        if wanted.contains(&pid) {
                            self.by_pid.entry(pid).or_insert_with(|| e.path().to_string_lossy().to_string());
                        }
                    }
                }
            }
        }
        self.by_pid.retain(|p, path| {
            wanted.contains(p) && Path::new(path).exists()
        });
    }

    /// Send one datagram for an affordable prefix of sorted, deduplicated pages.
    /// Return only what was enqueued, not a claim that any page became resident.
    pub fn dispatch(&mut self, tgid: u32, pages: &[u64], budget: &mut IoBudget) -> Option<(usize, &[(u64, u64)])> {
        let n = pages.len().min(budget.tokens as usize);
        if n == 0 { return None; }
        // Controller ticks are too infrequent to service the return queue under load.
        self.poll_msgs();
        let path = self.by_pid.get(&tgid)?;
        let segs = &mut self.segments;
        segs.clear();
        let mut count = 0;
        for &va in &pages[..n] {
            if let Some(last) = segs.last_mut() {
                if last.0.checked_add(last.1) == Some(va) {
                    last.1 += 4096;
                    count += 1;
                    continue;
                }
            }
            if segs.len() == 1024 { break; }
            segs.push((va, 4096));
            count += 1;
        }
        let buf = &mut self.req_buf;
        buf.clear();
        buf.reserve(8 + segs.len() * 16);
        buf.extend_from_slice(&2u32.to_le_bytes());
        buf.extend_from_slice(&(segs.len() as u32).to_le_bytes());
        for &(start, len) in segs.iter() {
            buf.extend_from_slice(&start.to_le_bytes());
            buf.extend_from_slice(&len.to_le_bytes());
        }
        if let Err(e) = self.sock.send_to(buf, path) {
            self.send_errs += 1;
            match e.kind() {
                std::io::ErrorKind::WouldBlock => self.queue_full += 1,
                std::io::ErrorKind::NotFound | std::io::ErrorKind::ConnectionRefused => self.peer_gone += 1,
                _ => (),
            }
            return None;
        }
        self.submitted += 1;
        budget.tokens -= count as f64;
        Some((count, segs))
    }
}

// ---------------------------------------------------------------------------
// IO 令牌桶(规格 §5.4):预取 ≤ io_share × 设备随机读能力;major p90 AIMD。
pub struct IoBudget {
    max_rate_per_s: u64,
    rate_per_s: u64,
    pub tokens: f64,
    pub capacity_s: f64,
    idle_p90_ns: u64,
    window_major_p90: u64,
}

impl IoBudget {
    pub fn new(device_iops: u64, io_share: f64, idle_p90_ns: u64) -> IoBudget {
        let rate = (device_iops as f64 * io_share) as u64;
        IoBudget {
            max_rate_per_s: rate,
            rate_per_s: rate,
            tokens: rate as f64,
            capacity_s: 2.0,
            idle_p90_ns,
            window_major_p90: 0,
        }
    }

    pub fn set_window_major_p90(&mut self, p90: u64) {
        self.window_major_p90 = p90;
    }

    /// 每 tick(1s)调用:AIMD 调速
    pub fn tick(&mut self) {
        if self.window_major_p90 > self.idle_p90_ns.saturating_mul(2) {
            self.rate_per_s = (self.rate_per_s / 2).max(64); // MD
        } else {
            let up = self.rate_per_s + (self.max_rate_per_s - self.rate_per_s) / 8;
            self.rate_per_s = up.min(self.max_rate_per_s); // AI
        }
        self.tokens = (self.tokens + self.rate_per_s as f64)
            .min(self.rate_per_s as f64 * self.capacity_s);
    }

    pub fn rate_per_s(&self) -> u64 {
        self.rate_per_s
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn dispatch_drains_feedback_and_only_charges_enqueued_pages() {
        let dir = std::env::temp_dir().join(format!("ps-agent-test-{}", std::process::id()));
        std::fs::create_dir(&dir).unwrap();
        let daemon = dir.join("d");
        let agent = dir.join("a");
        let peer = UnixDatagram::bind(&agent).unwrap();
        let mut am = AgentM::from_socket(UnixDatagram::bind(&daemon).unwrap()).unwrap();
        am.by_pid.insert(7, agent.to_str().unwrap().into());
        let mut budget = IoBudget::new(2, 1.0, 1);
        let pages = [4096, 8192, 16384];

        assert!(am.dispatch(8, &pages, &mut budget).is_none());
        am.by_pid.insert(8, dir.join("missing").to_str().unwrap().into());
        assert!(am.dispatch(8, &pages, &mut budget).is_none());
        assert_eq!((am.send_errs, am.peer_gone), (1, 1));
        assert_eq!(budget.tokens, 2.0);

        // A real ACK waiting between controller polls must not sit there until the next tick.
        let mut ack = [0u8; 32];
        ack[..4].copy_from_slice(&3u32.to_le_bytes());
        ack[4..8].copy_from_slice(&7u32.to_le_bytes());
        ack[8..16].copy_from_slice(&8192u64.to_le_bytes());
        ack[16..24].copy_from_slice(&4096u64.to_le_bytes());
        peer.send_to(&ack, &daemon).unwrap();
        let (count, segs) = am.dispatch(7, &pages, &mut budget).unwrap();
        assert_eq!((count, segs), (2, &[(4096u64, 8192u64)][..]));
        assert_eq!(am.submitted, 1);
        assert_eq!((am.acks, am.req_bytes, am.adv_bytes), (1, 8192, 4096));
        assert_eq!(budget.tokens, 0.0);
        assert!(am.dispatch(7, &pages, &mut budget).is_none());
        let mut wire = [0u8; 32];
        assert_eq!(peer.recv(&mut wire).unwrap(), 24);
        assert_eq!(&wire[..8], &[2, 0, 0, 0, 1, 0, 0, 0]);
        assert_eq!(u64::from_le_bytes(wire[8..16].try_into().unwrap()), 4096);
        assert_eq!(u64::from_le_bytes(wire[16..24].try_into().unwrap()), 8192);
        std::fs::remove_dir_all(dir).unwrap();
    }
}
