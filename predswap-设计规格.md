# predswap 设计规格（v0.2 + P1 修订，见附录 A）

**日期：** 2026-11-19
**状态：** Phase 0 完成（探测报告）；**P1 已交付并通过出口条件**（实现修订见附录 A）。下一阶段 P2：M0–M3 预测器 + 选择器。
**对标：** AMD MEXT（《AMD-MEXT-技术洞察报告.md》）。借鉴 FarSight、xTier、ArtMem（各自洞察报告 + 《内存分层方案对比.md》）。
**问题层次：** 内存分层里的**闪存 swap 预取 + 冷页卸载**（DRAM↔SSD）；和 FarSight / MEXT 一样属于预取侧问题，不是 CXL/NUMA 页放置。

---

## 1. 目标与需求

### 1.1 一句话

不打内核补丁，在 Kunpeng/openEuler 22.03（内核 5.10，cgroup v1）上做一个 MEXT 式系统：eBPF 双 entry kprobe 观测 swap 缺页流，按页龄把冷匿名页 `MADV_PAGEOUT` 换出到 SSD、靠 cgroup 限额压力释放 DRAM，用在线自选的模型族预测即将缺页的已换出页，经目标进程内的 LD_PRELOAD agent 用 `madvise(MADV_WILLNEED)` 预取进 swap cache。同一台机器、同一 DRAM 预算下，性能超过 Linux 原生 swap 和 TMO 式主动回收。

### 1.2 已确认的需求

| 项 | 决定 | 备注 |
|---|---|---|
| 慢层 | 闪存 swap | 实测 qd1 5 562 IOPS / 0.166ms，qd32 19 085 IOPS（探测报告 §2-10） |
| 平台 | Kunpeng 920 + openEuler 22.03 SP4 / 5.10，cgroup v1 | v0.1 写的"24.03"是 ENV 记录错误 |
| 内核改动 | 不打补丁；eBPF + 现有 syscall/cgroup；`.ko` 仅后备 | |
| 预测引擎 | 模型族在线自选（启发式 + 小 NN） | 不要求离线训练 |
| 成功标准 | 同机相对基线（§8.3 预注册门槛） | 不和 MEXT 自报数字比 |
| 负载 | memcached（nginx 前置）、GAP PR/BFS、XGBoost、合成微基准 | |
| 语言 | eBPF CO-RE（C）+ Rust 用户态 + agent.so | |

### 1.3 非目标（v1）

CXL/NUMA 放置；文件页分层；THP（实验统一 never）；跨节点池化；x86 PEBS/AVX512。

---

## 2. 从参考系统继承什么

| 来源 | 继承 | 不继承（原因） |
|---|---|---|
| MEXT | 遥测→AI 引擎→执行者三件套；卸载反馈式、回流预测式；模型族在线选择；RAM View 6 档页温度（<15s/<1min/<2min/<3min/<5min/>5min，报告 §2.2） | 专用裸盘、禁其他 swap、关 Secure Boot、mextd 不可停（§2.3）。本设计全动作是建议性 syscall，守护进程死了退化成原生 swap |
| FarSight | 只用缺页序列预测；后继表（future map）；小词表 token；推理藏在 I/O 等待里 | 内核内推理、5.5K 行内核改动（§3.1）、AVX512、离线 trace 训练 |
| FarSight 缺口 | 补论文有、代码无的 PC 通道（§4.1）——但 arm64 BPF 取用户 PC 的方法待 P1 验证，M4 先不带 PC | |
| xTier | 双向不同判据（回流预测、判冷确定性）；逐页冷静期；按地址排序合并成段再批量下发（对比文档 §1.3） | eBPF 内 INT8 推理（SSD 预算宽，用户态够）；按负载离线训练 |
| ArtMem | 直方图容量反推阈值（从最老桶累加，凑够要腾的量，§5）；log2 桶 | 改内核；struct page 死空间 |
| 本机实证（探测报告） | H 余量机制必需（WILLNEED 在限额处静默失效，§2-6）；PAGEOUT 后 swap cache 逐出交给限额压力（§2-5） | |

**决策预算依据：** SSD 随机读 166µs，比 FarSight 面对的 RDMA（~2µs）宽两个数量级 → 推理放用户态，事件→下发 p99 ≤1ms，前瞻 d 覆盖该窗口（§5.4）。

---

## 3. 架构

```mermaid
flowchart LR
  subgraph K["内核（不改源码）"]
    KP1["kprobe do_swap_page"]
    KP2["kprobe swapin_readahead"]
    PM["/proc/pid/pagemap"]
    AG["page_idle/bitmap"]
    SC[("swap cache")]
  end
  subgraph APP["目标进程"]
    AGT["predswap-agent.so<br/>(LD_PRELOAD)"]
  end
  KP1 -->|"ringbuf"| D["predswapd 遥测"]
  KP2 -->|"miss 标记"| D
  PM --> D
  AG --> O["卸载控制器"]
  D --> M["模型族 + 选择器"]
  D --> FB["反馈: 命中/浪费/迟到"]
  FB --> M
  M -->|"unix socket: 预取请求"| AGT
  AGT -->|"madvise WILLNEED"| SC
  O -->|"process_madvise PAGEOUT"| K
```

| 组件 | 形态 | 职责 | MEXT 对应 |
|---|---|---|---|
| `predswap-bpf` | eBPF CO-RE（C） | 双 kprobe 事件流，按托管 cgroup id 过滤，ringbuf 输出 | MEXT Driver |
| `predswapd` | Rust 守护进程 | 遥测状态、模型族、选择器、卸载控制器、IO 预算、录制 | AI Engine + mextd |
| `predswap-agent.so` | C，LD_PRELOAD 注入目标进程 | 收预取请求，进程内 `madvise(MADV_WILLNEED)`（探测报告 §2-2/4：跨进程 WILLNEED 在 5.10 不可用） | — |
| `predswapctl` | Rust CLI | 状态/温度分布/改配置/开关成员；与 daemon 走 unix socket JSON lines | RAM View + mext-config |
| `predswap-record` | 工具 | 录事件流 + PAGEOUT 批次 + agent 请求 → trace 文件；离线 replay 喂给各成员出准确率/覆盖率/迟到率 | — |
| `predswap.ko`（后备） | 可加载模块 | 仅当 eBPF kprobe 不可用 | — |

predswapd 线程：**ingest**（钉核：ringbuf → 状态 → M0–M3 推理 → 派发，每事件预算 30µs）+ **background**（M4 训练、pagemap 对账、页龄扫描、卸载控制）。合计 CPU 预算见 §7。

---

## 4. 遥测

### 4.1 swap 事件（主信号）

两个 entry kprobe（均已在探测报告 §2-8 验证），**不用 kretprobe**（该内核上 kretprobe 会挤掉同函数的 kprobe 事件，探测报告 §2-9）：

- `do_swap_page(vmf)`：每次访问换出页必经（WILLNEED 只装 swap cache 不建映射，PTE 仍是 swap 项；探测报告 §2-3 证实）。读 `vmf->address`；
- `swapin_readahead(entry, gfp, vmf)`：仅 cache miss（major）路径调用。同读 `vmf->address` 作 miss 标记。注意 WILLNEED 走 `read_swap_cache_async` 逐页，**不经过**此函数（`code/ArtMem/kernel/mm/madvise.c:187-217`），预取自身不产生 miss 事件。

事件结构（ringbuf，32B；buffer 默认 64MB——实测负载 7s 产生 27K 事件 ≈ 4K/s，64MB 覆盖分钟级突发；满则丢事件并累加 `ev_dropped`，绝不阻塞缺页路径）：

```c
struct swap_ev {
    u64 ts_ns;      // kprobe 时刻
    u64 va;         // PAGE_MASK 后的地址
    u64 cgid;       // BPF map managed_cgids 过滤
    u32 tgid, tid;  // 多线程按 tid 分流
    u8  kind;       // EV_SWAPIN(do_swap_page) | EV_MISS(swapin_readahead)
    u8  _pad[3];
};
```

**分类规则（合并到"一次访问"，探测报告 §2-8 验证了原始计数）**：VM_FAULT_RETRY 会使同一 (tid,va) 产生第二次 do_swap_page（慢事件后跟一次快的），所以：

| 观测序列（同 tid+va，窗口 ≤50ms） | 分类 |
|---|---|
| do_swap_page + MISS + do_swap_page | **major 缺页**（真读盘；两次事件间隔 ≈ IO 等待，代替 lat_ns） |
| do_swap_page + do_swap_page（无 MISS，间隔 > 200µs 的重试对） | **迟到**（预取在途：在途 folio 已入 swap cache，故 lookup 命中、不调 swapin_readahead，但页锁未释放，缺页 RETRY 等待。**P1 实现修正**：v0.2 曾写成"MISS+配对"，错——在途页在 cache 里，无 MISS） |
| 单条 do_swap_page，无 MISS、无重试对 | **及时命中**（预取或残留 cache） |
| MISS 无 do_swap_page 前导 | 内核 readahead 的簇内页，丢弃 |

**归因前提：** 运行时设 `page-cluster=0`、`vma_ra_enabled=false`（本机两个旋钮都在，探测报告 §1），退出恢复；内核预读能力由 M1 覆盖，swap cache 命中全部归因于自己。

**用户 PC 通道：** arm64 BPF 里读用户态 PC（`task_pt_regs`）的方法待 P1 验证；M4 默认仅 VPN-delta token，PC 验证通过后作为增强通道（λ_PC 消融在 §8.4）。

### 4.2 换出状态

托管进程的"已换出页集合"：
- 自己的 PAGEOUT 批次直接记账；
- 内核自主回收靠周期 pagemap 对账发现（PM_SWAP 位，私有匿名页 root 可见——探测报告 §2-7；**注意 MAP_SHARED 匿名 = shmem 换出后 pagemap 读全 0**，负载选择避开）；
- 全量对账周期 30s，每轮读 ≤64MB pagemap；
- 预测只对"已换出 + 无在途预取"的页派发。

### 4.3 页龄（AgeSource = page_idle，已确认）

后端固定为 `/sys/kernel/mm/page_idle/bitmap`（CONFIG 已开，探测报告 §1；DAMON/MGLRU 本机都没有）。
- PFN 从 pagemap 取，access bit 标记 idle 后下轮扫描判龄；换出页跳过（页不在内存）；
- 输出每页 log2 秒桶 + MEXT 口径 6 档温度分布；
- **成本核算：** 24GB ≈ 6.3M 页，全扫 = pagemap 读 48MB + bitmap 读写 ~2MB；扫描周期 10s（对齐 MEXT 最细档 <15s），单次扫描 CPU ≤0.05 核·s，只扫托管进程的地址空间而非全机。

---

## 5. 预测引擎（模型族）

### 5.1 接口

```rust
trait Predictor {
    fn observe(&mut self, acc: &Access, ledger: &mut Ledger);
    fn predict(&mut self, acc: &Access, d: usize, out: &mut Vec<Vpn>);
    fn mem_bytes(&self) -> usize;
}
```

`Access` 是 §4.1 合并后的一次访问。所有成员对每个事件 observe；**predict 有每事件预算**（见 5.2/5.3），只有选择器分到额度的成员的预测会真正下发。

### 5.2 成员

| 成员 | 机制 | 针对模式 | 状态上限 | 每事件成本上限 |
|---|---|---|---|---|
| M0 Null | 不预测 | 不可预测负载兜底 | 0 | 0 |
| M1 Stride | 每 (tid,VMA) 记 4 次缺页 VPN 差；连续 2 次相同步长 s → 预测 va+k·s | 顺序/定步长扫描；取代内核预读 | 每 VMA 64B | ~0.1µs |
| M2 Successor | FarSight future map 用户态版：VPN→最近 2 个后继（LRU 计数淘汰）；沿链走 d 步 | 指针追逐、图遍历 | 每项 24B，总量 ≤托管内存 0.5% | ~1µs |
| M3 Co-evict | 同一 PAGEOUT 批次中地址相邻的页为一组；组内页缺页时预取其余页。**共访问率定义：** 每组维护 16-bit 计数，组内页被访问则整组计数右移衰减后计 1；`共访问率 = 计数/16`，低于 1/16 的组失效删除 | 一起冷一起热的数据结构（memcached slab、图分区） | 每组 16B+位图 | ~0.2µs |
| M4 TinySeq | 2 层 GRU，隐维 32（约 1.2 万参数），输入 = 最近 T=32 次访问的 VPN-delta token（K=64：0–61 表示 ΔVPN∈[−31,−1]∪[1,31]，62=走 M2 第一后继，63=不预测），纯 Rust + **SVE2** 内积 | 有程序逻辑、无简单规则 | 参数 ≤64KB | 每事件 ≤10µs（d≤4；事件率>20K/s 时按 1/8 采样） |

**M4 每事件预算是硬上限**（v0.1 评审结论：d=32 全量影子推理在 6.7K events/s 时即 >4 核）。超预算成员本 epoch 降级为 observe-only。

**M4 在线训练：** background 线程滑窗 next-token SGD，每 60s 一轮，每轮采样 8K 长度 32 的窗口、≤200ms CPU（v0.1 的 64K×200ms 算不过账，评审结论 3）；冷启动 50K 事件不参与选择。P3 开头用实测定稿。

### 5.3 反馈与选择器

每条预测记入影子账本 `(vpn, member, t_pred)`，命中窗口 W=2s：
- **命中：** W 内该 vpn 出现"及时命中"（§4.1 表第 3 行）。**在被访问前又被换出/逐出 cache 的算浪费**（不会表现为及时命中，自动归入）；
- **浪费：** W 内无访问且已下发；
- **迟到：** 下发过但事件分类为"迟到"。

每 epoch（1s）：`U_m = hits_m − λ·wasted_m`，λ 默认 0.5（P1 按 qd1/qd32 实测回调）。选择器 Hedge：`w_m ← w_m·exp(η·U_m/N)`，η=0.5，归一化后按权重分配下 epoch 预取额度。**多条预测同一 vpn 时全命中记功、全浪费记过**（选择器要度量预测质量，与派发重复无关）；实际派发去重一次。M0 权重最高 = 关预取，是 G2 的保底。

全局指标（predswapctl）：准确率 = 下发且及时命中/下发；覆盖率 = 及时命中的 swap-in/全部 swap-in；迟到率。

### 5.4 前瞻距离、IO 预算与派发

- **d 自适应：** 使"事件间隔 p50 × d ≥ 预取完成延迟 p90"，d ∈ [1,32]；预取完成延迟 = 派发→该页"及时命中"的最短间隔统计；
- **IO 预算：** 实测 qd32 19 085 IOPS，预取令牌桶默认 40%（≈7.6K 读/s）。major 的 p90 IO 等待超空载基线 2 倍时额度减半，恢复后线性回升（AIMD）；
- **派发路径：** predswapd 按 VPN 排序合并成段 → unix dgram socket 发给目标进程的 agent → agent `madvise(WILLNEED)`。dgram 天然丢弃过载请求（背压由 IO 令牌桶承担）。段级去重：在途段内 vpn 不重复。

---

## 6. 卸载控制器

判冷不用模型，只用确定性页龄（不对称原则：误判回流浪费一次读，误判换出每次访问都付 166µs）。

- **容量目标：** 目标变量 = cgroup `memory.usage_in_bytes`（v1，实测有效，探测报告 §1）≤ `B − H`，H = max(2%·B, 64MB)。**H 是机制必需**：限额处 WILLNEED 静默无效（探测报告 §2-6）；
- **阈值：** ArtMem 式直方图反推（页龄 log2 桶从最老往新累加，累计 ≥ `usage − (B−H)` 时的桶即判冷阈值）；
- **执行：** 阈值以上的页按地址合并成段 → 跨进程 `process_madvise(MADV_PAGEOUT)`（5 参，flags=0，返回建议字节数；探测报告 §2-1），每轮上限 `max_pageout_mb=256MB`；
- **物理释放：** PAGEOUT 只解除映射，folio 留在 swap cache（探测报告 §2-5）；实际 DRAM 释放由 cgroup 限额对 swap cache 的逐出完成——这是特性不是缺陷：被逐出的只是"已经换出过的页"的 cache 副本，顺序仍由我们的 PAGEOUT 决定。pagemap 对账监控内核自主换出占比，>20% 告警；
- **无 `memory.reclaim`、无 PSI（cgroup v1，探测报告 §1）：** 护栏改用 memory.stat 的 pgmajfault 斜率——每 epoch 新增 major 超过阈值（默认 500/s）即暂停主动换出，等命中恢复；
- **冷静期：** 最近 C=30s 内 swap-in 过的页不进候选（xTier 冷静期）；C 内又被访问的页计乒乓；
- **周期 1s。** 关闭预测只跑此控制器 = 基线 B2。

---

## 7. 非功能需求

| 项 | 要求 | 依据 |
|---|---|---|
| CPU | predswapd 平均 ≤1 核、峰值 ≤2 核；eBPF 每事件 ≤2µs；agent ≤0.1 核 | MEXT 自称 2 核；xTier <4% |
| 内存 | predswapd RSS ≤ 托管内存 1% + 64MB（含 §5.2 各成员上限之和） | FarSight 上限 6.25%，取更紧 |
| 正确性 | `kill -9 predswapd` 应用无错误，退化为原生 swap | 与 MEXT"不可停"相反 |
| 权限 | root；BPF 加载 CAP_BPF/CAP_SYS_ADMIN；agent 注入 = 负载启动时 LD_PRELOAD（`systemd-run --scope -p MemoryMax=B` 同一命令里带环境变量） | |
| Secure Boot | 不要求关 | |
| swap 设备 | 实验期只启用 `/home/swapfile`（关 4G 分区，两设备并存时内核优先 4G 分区，探测报告 §1）。25% 档工作集上限受 swap 容量约束：WS ≤ B + swap总量 − 余量 → WS ≤ ~18GB；如需更大 WS 扩 swapfile | ENV swapon |
| 内核预读 | `page-cluster=0`、`vma_ra_enabled=false`，退出恢复 | §4.1 归因 |
| SSD 磨损 | 报告 pswpout 速率与折算 DWPD | MEXT 报告 §5.1 |
| 配置 | `/etc/predswap.toml`：cgroup 路径、H、λ、η、W、C、成员开关、IO 份额 | |
| 可观测 | `predswapctl status`：各 cgroup 常驻/换出、6 档温度、成员权重/准确率/覆盖率/迟到率、IO 额度、`ev_dropped`、乒乓计数 | MEXT RAM View |
| 进程发现 | 监控 cgroup tasks 文件（v1）+ pidfd 轮询退出；exec 后 pidfd 触发 agent 重连 | |

---

## 8. 评估

### 8.1 基线

| 编号 | 配置 |
|---|---|
| B0 | 全 DRAM，性能参照 |
| B1 | 原生 swap：`memory.limit_in_bytes=B`，默认 page-cluster=3 + vma_ra 开 |
| B2 | predswapd 只开卸载控制器（预测关闭），= TMO 式主动回收 |
| P | predswapd 全功能 |

（v0.1 的 B1'/B3——MGLRU、etmem——本机没有，删除，探测报告 §1。）

统一控制：仅 swapfile；THP never；`memory.swappiness=100`（每组，v1 按组设）；`systemd-run --scope -p MemoryMax=B` 限额并注入 agent；每 run 重启负载。

### 8.2 负载与预算

- memcached（nginx 前置；S1 稳态 Zipf、S2 相变轮转，拓扑沿用《nginx-ml-tiering-experiment-design.md》§2.2）、GAP PR/BFS、XGBoost；
- 合成微基准：顺序扫描（M1）、定步长（M1）、固定随机链表（M2/M4）、成块冷热（M3）、纯随机（期望收敛 M0）；
- B ∈ {100%, 75%, 50%, 25%} × WS；**WS ≤18GB**（§7 swap 约束），单 NUMA 节点内完成（每节点 31GB，排除跨节点延迟）；
- 每格 ≥3 reps 交错；统计方法沿用 nginx 方案 §9。

### 8.3 验收门槛（预注册）

X = 10%（与 nginx 方案一致）。pilot 前可改，之后不可。

| 门槛 | 条件 |
|---|---|
| G1 | B=50% 时 P 比原生最好（=B1，无 MGLRU/etmem 可比）快 ≥10%，≥2/3 负载成立 |
| G2 | 任何负载/预算下 P 不比 B1 慢 >3%（纯随机微基准专项检验） |
| G3 | P 比 B2 快 ≥10%（预测本身的贡献） |
| G4 | SLO 下 DRAM_min(P) ≤ 0.9 × DRAM_min(B1)（二分法沿用 nginx 方案 §6） |
| G5 | §7 全部满足 |
| G6 | 运行中 `kill -9 predswapd` 10 次，负载零错误 |

诊断指标（不设门槛）：各成员准确率/覆盖率/迟到率、权重轨迹、乒乓、IO 占比、pswpin/out。

### 8.4 消融

单成员 vs 全族+选择器（自选 ≥ 事后最佳单成员的 90%）；M4 去训练（只用 M2 捆绑）量化在线学习价值；PC 通道（P1 验证可用后）；λ、η、W、H、C、d 各扫一维。

---

## 9. Phase 0 结论（已完成，2026-11-19）

全部探测项已定：内核 5.10/cgroup v1/无 DAMON、MGLRU、etmem、reclaim、PSI、force_swapin；BTF + kprobe + BPF libbpf 在；idle page tracking 在；双 kprobe 事件模型与 WILLNEED/PAGEOUT 机制实测通过。细节与证据见探测报告（本仓库根目录）。

**遗留两项移入 P1 入口：**
1. eBPF kprobe 程序实际加载（机器无 clang/bpftool，需 yum 安装或本地生成 vmlinux.h/BPF ELF 后上传；libbpf.so 0.8.1 已在）；
2. page_idle 功能实测（PFN→bitmap 判龄走通）。

## 10. 里程碑

| 阶段 | 交付 | 出口条件 |
|---|---|---|
| P1 | 双 kprobe 事件流 + 归并分类器 + agent + 卸载控制器 + predswapctl + predswap-record/replay（=B2 可跑） | B2 在 50% 预算不比 B1 差；G5、G6 过；replay 管线能重放同一条 trace 复现指标 |
| P2 | M0–M3 + 选择器 + IO 预算 | 微基准上各成员按预期命中；G2 过；replay 上单成员准确率达标 |
| P3 | M4 在线训练（PC 通道视 P1 验证结果） | §8.4 消融跑通；M4 训练一轮实测 CPU 在预算内 |
| P4 | 完整评估 | G1–G6 报告（含负结果） |

代码：`code/predswap/`（`bpf/`、`daemon/`、`agent/`、`ctl/`、`bench/`、`tools/record+replay`），独立仓库。

## 11. 风险

| 风险 | 影响 | 应对 |
|---|---|---|
| SATA SSD 19K IOPS，qd 扩展性 3.4× | 预取额度小、误预测代价高；结果不能外推 NVMe | 同机相对比较（已确认）；报告写明介质 |
| 用户态事件→派发 p99 >1ms | 迟到率高 | 增大 d；ingest 钉核 |
| 后继表冷启动（每页首次缺页必 miss） | 覆盖率上不去 | M1、M3 不依赖逐页历史，兜底 |
| 多线程缺页序列交错 | M2/M4 准确率降 | 按 tid 分流（事件已带） |
| agent 注入改变负载环境 | memcached/nginx 类服务可用环境变量启动，无侵入配置；对无法注入的进程（已有静态二进制）则不托管 | 执行器后备 `.ko`；G2 用同注入方式的对照组隔离 LD_PRELOAD 本身的影响 |
| 内核自主回收竞争 | 换出乱序 | 留 H；对账监控 >20% 告警 |
| cgroup 限额处预取静默失效 | 效果消失且无报错 | H 机制化；predswapctl 显示"WILLNEED 有效字节/请求字节"比值，<90% 告警 |
| 全局 sysctl 影响非托管进程 | 干扰其他负载 | 实验机专用 |

---

## 附录 A：P1 实现修订记录（2026-11-19，P1 完成后回填）

P1 已交付并通过出口条件（代码 `code/predswap/`，证据见各 bench 脚本输出）。实现过程中对 v0.2 的修正与补充：

| # | 修正/发现 | 依据 |
|---|---|---|
1 | §4.1"迟到"分类修正：在途预取页已入 swap cache，缺页无 MISS，表现为无 miss 的间隔重试对（表内已改） | `lookup_swap_cache` 先于 miss 分支（`code/ArtMem/kernel/mm/memory.c:3527`）+ T1 实测事件序列 |
| 2 | 事件结构定稿 32B（自然对齐尾部填充，v0.2 草写 28B 是错的）；kind 字段过滤、tgid 过滤表由 bridge stdin 行协议维护（v0.2 的 cgid 过滤在 cgroup v1 不可靠，改按 tgid） | T1 事件流验证 |
| 3 | bridge 需要 `RLIMIT_MEMLOCK=unlimited`（64M 下 EPERP）；daemon 启动时自行 setrlimit | 探测中复现 |
| 4 | page_idle 实操三点：bitmap 必须**读写**打开（只读打开时 write_at 静默失败，页龄永不清零——本次最大调试成本）；被测页必须已在 LRU（pagevec 未排空的新页写入被静默跳过，触碰 ≥15 页促使排空）；读位判龄协议 = 每周期读旧位→回写 1 重新武装 | `page_idle.c` 源码 + 受控实验（bench/idle_test.py） |
| 5 | 判冷阈值补最小页龄钳制 15s（`min_epochs = 15/scan_period`），对齐 MEXT 最细档；否则 scan 周期 5s 时 5s 空闲页即被换出，控制器变成额外换出 | 78% 预算首轮 B2 比 B1 多 4.4% major 的归因 |
| 6 | major 护栏的"速率"必须按秒折算（按扫描周期计会让 500/s 阈值变成 500/5s，形同虚设的反向错误）；重换页负载下绝对阈值会永久暂停控制器，改为可配置并在实验中用 3000/s | B2 首轮全程 paused=true 复现 |
| 7 | B2@50% 出口条件通过：3 reps 交错，B2 major 数均值 −1.0%（−2.5%/−0.7%/+0.2%），ops −0.4%（噪声级）。但该负载（tiers）下候选稀缺（冷却+页龄双闸后 ~700 页/tick），B2 ≈ B1 + 微扰——真实判别力要等 P2 预测开启后在 SLO/budget 扫描中体现 | bench/b1b2_50.sh |
| 8 | 事件流端到端验证：99.9% 落在已知工作集区域、miss 计数 ≈ getrusage majflt、swapin 计数 = 触碰页数 + RETRY 重试数（配对机制实证）；replay 同 trace 复现分类（100% 总量一致）；major 的 io_wait p50=175µs 与空载盘延迟吻合 | T1 验证 + daemon replay |
| 9 | agent 端到端验证：预取半区 majflt 8463→4160（近半），ACK 报告有效字节/请求字节 | T2 验证 |
| 10 | 机器事实补充：NUMA 节点 1 物理地址在 1TB 起，pagemap PFN ≈ 2.7 亿是正常的（勿当越界值误判）；`/proc/pid/status` 的字段是 `Tgid:`（小写 g，非 `TGid:`） | p0/b2 调试 |

P1 出口条件核销：B2@50% 不劣于 B1 ✓（#7）；G5 ✓（daemon 0.039 核、RSS 4.3MB，见 bench/g5.sh）；G6 ✓（10 次 `kill -9` + 重启，负载 100s 跑完 rc=0、`tiers verify bad=0` 数据完好，见 bench/g6.sh）；replay 复现 ✓（#8）。P1 交付物：`bpf/`、`bridge/`、`agent/`、`daemon/`、`bench/`（synth + 全部验证脚本），构建与运行方法见 `code/predswap/README.md`。
