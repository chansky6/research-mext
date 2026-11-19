// ctl socket(status 服务端 + 客户端,规格 §7 可观测)
use std::io::{Read, Write};
use std::os::unix::net::UnixListener;
use std::os::unix::net::UnixStream;

pub const CTL_SOCK: &str = "/tmp/predswap/ctl.sock";

pub fn start_server() -> std::io::Result<UnixListener> {
    let _ = std::fs::remove_file(CTL_SOCK);
    std::fs::create_dir_all("/tmp/predswap")?;
    UnixListener::bind(CTL_SOCK)
}

pub fn serve_one(l: &UnixListener, answer: impl Fn() -> String) {
    match l.accept() {
        Ok((mut s, _)) => {
            let mut req = [0u8; 64];
            if let Ok(n) = s.read(&mut req) {
                if &req[..n.min(n)] == b"status" || req.starts_with(b"status") {
                    let a = answer();
                    let _ = s.write_all(a.as_bytes());
                } else {
                    let _ = s.write_all(b"unknown command\n");
                }
            }
        }
        Err(_) => {}
    }
}

pub fn client_status() -> String {
    let mut s = match UnixStream::connect(CTL_SOCK) {
        Ok(s) => s,
        Err(e) => return format!("ctl: connect failed: {e}\n"),
    };
    let _ = s.write_all(b"status");
    let _ = s.shutdown(std::net::Shutdown::Write);
    let mut out = String::new();
    let _ = s.read_to_string(&mut out);
    out
}
