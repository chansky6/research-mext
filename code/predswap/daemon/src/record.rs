// record/replay(规格 §3 predswap-record + P1 出口条件:同 trace 重放复现指标)
//
// 文件格式("PSREC1\n" 头之后):
//   记录类型 'E'(1B) + 原始事件 32B
//   记录类型 'P'(1B) + u64 ts + u32 tgid + u32 nseg + nseg×16B(段)
//   记录类型 'A'(1B) + 24B(Access:分类后访问,由 daemon 在 run 模式写入;
//              replay 模式重放 'E' 自行分类,与 'A' 对照做一致性校验)
use crate::event::{Access, AccessKind, SwapEv, EV_SZ};
use std::fs;
use std::io::{self, BufWriter, Write};
use std::path::Path;

pub struct Recorder {
    w: BufWriter<fs::File>,
}

impl Recorder {
    pub fn open(path: &Path) -> io::Result<Recorder> {
        let mut f = fs::File::create(path)?;
        f.write_all(b"PSREC1\n")?;
        Ok(Recorder { w: BufWriter::new(f) })
    }

    pub fn ev(&mut self, raw: &[u8; EV_SZ]) -> io::Result<()> {
        self.w.write_all(b"E")?;
        self.w.write_all(raw)
    }

    pub fn access(&mut self, a: &Access) -> io::Result<()> {
        let mut b = [0u8; 26];
        b[0] = b'A';
        b[1..9].copy_from_slice(&a.ts_ns.to_le_bytes());
        b[9..17].copy_from_slice(&a.va.to_le_bytes());
        b[17..21].copy_from_slice(&a.tgid.to_le_bytes());
        b[21] = a.kind as u8;
        b[22..26].copy_from_slice(&(a.io_wait_ns as u32).to_le_bytes());
        self.w.write_all(&b)
    }

    pub fn dispatch(&mut self, ts_ns: u64, tgid: u32, segs: &[(u64, u64)]) -> io::Result<()> {
        let mut b = vec![b'D'];
        b.extend_from_slice(&ts_ns.to_le_bytes());
        b.extend_from_slice(&tgid.to_le_bytes());
        b.extend_from_slice(&segs.iter().map(|s| s.1).sum::<u64>().to_le_bytes());
        b.extend_from_slice(&(segs.len() as u32).to_le_bytes());
        for &(s, l) in segs {
            b.extend_from_slice(&s.to_le_bytes());
            b.extend_from_slice(&l.to_le_bytes());
        }
        self.w.write_all(&b)
    }

    pub fn pageout(&mut self, ts_ns: u64, tgid: u32, segs: &[(u64, u64)], ret: i64) -> io::Result<()> {
        let mut b = vec![b'P'];
        b.extend_from_slice(&ts_ns.to_le_bytes());
        b.extend_from_slice(&tgid.to_le_bytes());
        b.extend_from_slice(&(ret as u64).to_le_bytes());
        b.extend_from_slice(&(segs.len() as u32).to_le_bytes());
        for &(s, l) in segs {
            b.extend_from_slice(&s.to_le_bytes());
            b.extend_from_slice(&l.to_le_bytes());
        }
        self.w.write_all(&b)
    }

    pub fn flush(&mut self) -> io::Result<()> {
        self.w.flush()
    }
}

pub enum RecItem {
    Ev(SwapEv),
    Access(Access),
    Pageout {
        #[allow(dead_code)]
        ts_ns: u64,
        #[allow(dead_code)]
        tgid: u32,
        ret: i64,
        segs: Vec<(u64, u64)>,
    },
    Dispatch {
        #[allow(dead_code)]
        ts_ns: u64,
        #[allow(dead_code)]
        tgid: u32,
        bytes: u64,
        segs: Vec<(u64, u64)>,
    },
}

pub fn read_all(path: &Path) -> io::Result<Vec<RecItem>> {
    let data = fs::read(path)?;
    if !data.starts_with(b"PSREC1\n") {
        return Err(io::Error::new(io::ErrorKind::InvalidData, "not a PSREC1 trace"));
    }
    let mut out = Vec::new();
    let mut p = 7usize;
    while p < data.len() {
        match data[p] {
            b'E' => {
                let end = p + 1 + EV_SZ;
                if end > data.len() { break; }
                let mut raw = [0u8; EV_SZ];
                raw.copy_from_slice(&data[p + 1..end]);
                out.push(RecItem::Ev(SwapEv::parse(&raw)));
                p = end;
            }
            b'A' => {
                let end = p + 26;
                if end > data.len() { break; }
                let ts = u64::from_le_bytes(data[p+1..p+9].try_into().unwrap());
                let va = u64::from_le_bytes(data[p+9..p+17].try_into().unwrap());
                let tgid = u32::from_le_bytes(data[p+17..p+21].try_into().unwrap());
                let kind = match data[p+21] {
                    0 => AccessKind::Hit,
                    1 => AccessKind::Major,
                    _ => AccessKind::Late,
                };
                let wait = u32::from_le_bytes(data[p+22..p+26].try_into().unwrap()) as u64;
                out.push(RecItem::Access(Access { ts_ns: ts, va, tgid, tid: 0, kind, io_wait_ns: wait }));
                p = end;
            }
            b'D' => {
                if p + 24 > data.len() { break; }
                let ts = u64::from_le_bytes(data[p+1..p+9].try_into().unwrap());
                let tgid = u32::from_le_bytes(data[p+9..p+13].try_into().unwrap());
                let bytes = u64::from_le_bytes(data[p+13..p+21].try_into().unwrap());
                let nseg = u32::from_le_bytes(data[p+21..p+25].try_into().unwrap()) as usize;
                let mut segs = Vec::with_capacity(nseg);
                for k in 0..nseg {
                    let q = p + 25 + k * 16;
                    if q + 16 > data.len() { segs.clear(); break; }
                    segs.push((
                        u64::from_le_bytes(data[q..q+8].try_into().unwrap()),
                        u64::from_le_bytes(data[q+8..q+16].try_into().unwrap()),
                    ));
                }
                out.push(RecItem::Dispatch { ts_ns: ts, tgid, bytes, segs });
                p = p + 25 + nseg * 16;
            }
            b'P' => {
                if p + 24 > data.len() { break; }
                let ts = u64::from_le_bytes(data[p+1..p+9].try_into().unwrap());
                let tgid = u32::from_le_bytes(data[p+9..p+13].try_into().unwrap());
                let ret = u64::from_le_bytes(data[p+13..p+21].try_into().unwrap()) as i64;
                let nseg = u32::from_le_bytes(data[p+21..p+25].try_into().unwrap()) as usize;
                let mut segs = Vec::with_capacity(nseg);
                for k in 0..nseg {
                    let q = p + 25 + k * 16;
                    if q + 16 > data.len() { segs.clear(); break; }
                    segs.push((
                        u64::from_le_bytes(data[q..q+8].try_into().unwrap()),
                        u64::from_le_bytes(data[q+8..q+16].try_into().unwrap()),
                    ));
                }
                out.push(RecItem::Pageout { ts_ns: ts, tgid, ret, segs });
                p = p + 25 + nseg * 16;
            }
            _ => break, // 未知记录:截断
        }
    }
    Ok(out)
}
