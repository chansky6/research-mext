# predswap Phase 0 平台探测报告

**日期：** 2026-11-19
**对象：** 192.168.90.7（Kunpeng 920，见 `ENV-192.168.90.7.md`）
**结论：** GO。规格 `predswap-设计规格.md` v0.2 已按本报告全部修订。遗留两项（BPF 程序加载、page_idle 功能实测）移入 P1 入口条件。

---

## 1. 静态探测

| 检查项 | 结果 | 依据 |
|---|---|---|
| 内核 | **5.10.0-216.0.0.115.oe2203sp4**（openEuler 22.03 LTS SP4）——ENV 原记录"24.03"**错误**，已更正 | `uname -r` |
| CPU 特性 | SVE2/SME 可用（比规格 v0.1 假设的"只有 NEON"更好），M4 推理可用 SVE | `/proc/cpuinfo` Features |
| BTF | 存在（4.4MB） | `/sys/kernel/btf/vmlinux` |
| kprobe 目标 | `do_swap_page`、`swapin_readahead`、`lookup_swap_cache`、`force_shm_swapin_readahead` 均可挂（在 `available_filter_functions`，55 183 行） | tracingfs |
| cgroup | **v1**（无 `memory.reclaim`、无 `memory.pressure`）；`memory.limit_in_bytes` **实测有效**（ENV"手写不生效"记录错误）；`memory.swappiness` 每组可用；限额可运行时上调 | `/proc/mounts` + 动态测试 |
| DAMON / MGLRU / etmem / bpftool / bpftrace / clang | 全部没有 | ls / rpm |
| libbpf | **有** `/usr/lib64/libbpf.so.0.8.1`；gcc、make 有 | 需自带 clang + bpftool（yum 或本机交叉生成 vmlinux.h） |
| idle page tracking | `CONFIG_IDLE_PAGE_TRACKING=y` + `/sys/kernel/mm/page_idle/bitmap` 存在 → AgeSource 定为 page_idle | config + sysfs |
| openEuler 特有旋钮 | `/sys/kernel/mm/swap/vma_ra_enabled`（bool）、`kernel_swap_enable`、`page-cluster=3` → 可关内核预读做归因 | sysfs |
| `memory.force_swapin` | **不存在**（记忆中的 openEuler 接口在此内核无） | cgroup v1 memory 目录 grep |
| swap 布局 | 4G 分区（pri -2）+ /home/swapfile 16G（pri -3）**两个**都在启用，内核优先用 4G 分区 | `swapon --show` |
| EDR | 奇安信终端安全 `/opt/qaxsafe/qaxsafed` 常驻。**实测未干扰 ftrace/kprobe**（见 §3 教训） | ps |

## 2. 动态测试（核心机制验证）

测试程序 `/tmp/p0_int3.c`：A 96MB 私有匿名（逐页写）→ `MADV_PAGEOUT` → 192M cgroup 限额下触碰 B 192MB 逼出 swap cache → 限额上调 288M 留余量 → 进程内 `MADV_WILLNEED` A 后半 48MB → 分别触碰两半。

| # | 结论 | 依据 |
|---|---|---|
| 1 | **跨进程 `process_madvise(MADV_PAGEOUT)` 可用**，返回建议字节数 | 返回 67 108 864 = 64MB；5 参数调用（第 5 参 flags 必须传 0，否则 EINVAL） |
| 2 | **跨进程 `process_madvise(MADV_WILLNEED)` 不可用**：5.10 只接受 COLD/PAGEOUT（WILLNEED 是 5.15 加入的，见 `code/ArtMem/kernel/mm/madvise.c:1046` 对比） | EINVAL，advice=3 |
| 3 | **进程内 `madvise(MADV_WILLNEED)` 完全符合设计**：48MB 读回 swap cache（SwapCached +49MB），PTE 保持 swap 项（VmSwap 不变），触碰时 `majflt=0`、全 minor、0.059s（对照组 0.26s，1546 majflt），数据完好 | 多轮复跑，`/proc/meminfo`、pagemap、getrusage 三方一致 |
| 4 | **执行器改 LD_PRELOAD agent（进程内 WILLNEED）**，结论 2 的替代 | §3 |
| 5 | **PAGEOUT 不直接释放 DRAM**：folio 留在 swap cache（`reclaim_pages`→`shrink_page_list` 回写中 `__remove_mapping` 失败，`code/ArtMem/kernel/mm/vmscan.c:1746`）；实测 PAGEOUT 后 SwapCached +96MB。物理释放靠 cgroup 限额压力逐出 swap cache（实测逼压后 SwapCached 4kB） | meminfo + 源码 |
| 6 | **预取余量 H 是硬需求**：cgroup 顶限额时 WILLNEED **静默无效**（`read_swap_cache_async` 充值失败）——先 192M 逼压、再上调 288M 后 WILLNEED 才生效 | p0_int2（无余量，+0kB）vs p0_int3（有余量，+49MB）对照 |
| 7 | pagemap 的 PM_SWAP 位对私有匿名页可见（root）；**MAP_SHARED 匿名（=shmem）换出后读出全 0**——测试/负载须避开，也是 v0.1 Python 测试全零伪象的原因 | p0_int（C，MAP_PRIVATE）vs p0_dyn.py（MAP_SHARED） |
| 8 | **双 entry kprobe 事件模型验证通过**（不用 kretprobe）：`do_swap_page` 25 773 条 + `swapin_readahead` 1 184 条（≈majflt 801 + 逼压阶段 B 的内核换入），控制半区有 miss、预取半区 majflt=0 | tracing 实测 |
| 9 | kretprobe 在此内核有怪癖：`r:` 事件注册后同函数的 `p:` 事件被挤掉（ls 复核两次）→ **设计上不使用 kretprobe** | 见 §3 |
| 10 | SSD 随机 4K 读（实数据文件 + drop_caches）：qd1 **5 562 IOPS / median 0.166ms**、qd8 17 427、qd32 19 085。稀疏文件测盘是伪测（读洞不产生 IO，4µs"延迟"） | `/tmp/p0_iops.py`，swapfile 直接测被否 |

## 3. 调试教训（防止后续代理重蹈）

1. `process_madvise` 是 5 参 syscall，flags 漏传时寄存器残值触发 EINVAL；
2. ftrace 此内核 `kprobe_events` 每次 open+write **替换整表**：多探针必须一次 printf 写入（`\n` 分隔）；清理顺序 = 先 disable 各探针再清表（否则 EBUSY，属正常 ftrace 行为）；
3. trace 事件行格式是 `ps: (do_swap_page+0x0/0xdc4)`（冒号+空格），grep `':ps:'` 恒 0——本次排查中"EDR 清探针""僵尸探针"均为该 grep 假象 + 第 2 条的组合误判；
4. Python `mmap.mmap(-1, n)` 默认 **MAP_SHARED**（shmem），与 C 的 `MAP_PRIVATE` 语义不同；
5. 换出状态、SwapCached、VmSwap 三个口径要同时看：RSS 掉 ≠ DRAM 释放。

## 4. 规格影响（→ v0.2 已改）

- 执行器：预取 = LD_PRELOAD agent 进程内 `madvise(WILLNEED)`；卸载 = 跨进程 `process_madvise(PAGEOUT)`（跨进程也可用 agent，P1 定）；
- 遥测：`do_swap_page` + `swapin_readahead` 双 entry kprobe，按 miss 配对分类，无内核侧延迟（配对间隔即 IO 等待），无 retval；
- 卸载语义：PAGEOUT 选择页，物理释放交给 cgroup 限额压力；H 从"建议"升为"机制必需"；
- 删掉：memory.reclaim 兜底、PSI 护栏、MGLRU/etmem 基线、force_swapin 可选执行器、6.6/5.19+ 接口全部相关假设；
- 用户 PC 通道：arm64 kprobe 下取用户态 PC 的方法（BPF 读 `task_pt_regs`）待 P1 验证，M4 先按"仅 VPN-delta token"实现，PC 作为增强；
- 评估：swap 总量 20G 限制 25% 档的工作集上限（≤18GB），swapfile 需扩容或并用分区。
