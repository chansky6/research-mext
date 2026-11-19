// idle page tracking 扫描(规格 §4.3;后端唯一:page_idle/bitmap,Phase 0 §1 确认)
//
// 语义(kernel Documentation/admin-guide/mm/idle_page_tracking):
//   写 1 到某页的 bit = 标记 idle(清除 PTE accessed 位,开始跟踪);
//   读 bit:1 = 自上次标记以来未访问;0 = 访问过。
// 扫描周期 = config.scan_period_s;age 以"周期数"计数。
use std::collections::HashMap;
use std::fs;
use std::io;
use std::os::unix::fs::FileExt;

const IDLE_BITMAP: &str = "/sys/kernel/mm/page_idle/bitmap";

pub struct IdlePage {
    fd: fs::File,
    /// 上一轮已标记 idle 的 PFN(本轮读它们的结果)
    armed: HashMap<u32, ()>,
}

impl IdlePage {
    pub fn new() -> io::Result<IdlePage> {
        Ok(IdlePage {
            // 必须读写:写 1 才能标记 idle(只读打开时 write_at 静默失败,arm 永远不生效)
            fd: fs::OpenOptions::new().read(true).write(true).open(IDLE_BITMAP)?,
            armed: HashMap::new(),
        })
    }

    fn read_bit(&self, pfn: u32) -> io::Result<bool> {
        let word = pfn as u64 / 64;
        let mut b = [0u8; 8];
        self.fd.read_at(&mut b, word * 8)?;
        let v = u64::from_le_bytes(b);
        Ok(v & (1u64 << (pfn % 64)) != 0)
    }

    fn set_bits(&self, pfns: &[u32]) -> io::Result<()> {
        // 按 64 页字聚合,一次 write_at 一个字
        let mut words: HashMap<u64, u64> = HashMap::new();
        for &p in pfns {
            let w = p as u64 / 64;
            *words.entry(w).or_insert(0) |= 1u64 << (p % 64);
        }
        for (w, v) in words {
            let b = v.to_le_bytes();
            self.fd.write_at(&b, w * 8)?;
        }
        Ok(())
    }

    pub fn armed_len(&self) -> usize {
        self.armed.len()
    }

    /// 一轮扫描:对当前 resident PFN 集合,读上轮标记结果(1=idle 未触碰),
    /// 更新 age(调用方);再写 1 重新武装。返回 (pfn, 上轮是否 idle)。
    pub fn scan(&mut self, resident_pfns: &[u32]) -> io::Result<Vec<(u32, bool)>> {
        let mut out = Vec::with_capacity(resident_pfns.len());
        for &p in resident_pfns {
            let idle = if self.armed.contains_key(&p) {
                self.read_bit(p)?
            } else {
                true // 首轮:当作 idle,不惩罚
            };
            out.push((p, idle));
        }
        self.set_bits(resident_pfns)?;
        self.armed.clear();
        for &p in resident_pfns {
            self.armed.insert(p, ());
        }
        Ok(out)
    }
}
