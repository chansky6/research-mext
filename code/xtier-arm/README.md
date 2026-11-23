# xtier-arm — xTier 在 ARM (SPE) + DRAM/SSD 上的用户态复现

SPDX-License-Identifier: GPL-2.0

把 [xTier](https://github.com/a85tract/xtier)（上游克隆于 `../xtier-origin`）复现到
192.168.90.7（Kunpeng 920, openEuler 22.03 SP4, 5.10 aarch64，288 核 2 NUMA 节点，
SATA SSD）。三个硬约束：

1. **不改内核**（不 patch、不 insmod 模块）——只允许 syscall / procfs / perf；
2. **ARM 架构**：用 Arm SPE 替代 Intel PEBS 采样；
3. **无 CXL**：DRAM(快) + SSD-swap(慢) 两层替代 DRAM + CXL(远端 NUMA)。

## 为什么是"用户态端口"

xTier 的决策路径在内核（BPF perf_event 程序 + 内核模块调用 `xtier_migrate_range`）。
本机 5.10 内核上两条路都走不通：

- SPE 采样无法送进 BPF：PEBS 每个样本触发 `bpf_overflow_handler`，SPE 则是把
  记录写进 **aux 缓冲区**，由用户态解码（已验证：per-task 事件 + 用户态写
  `user_page->aux_offset/aux_size` 后 mmap aux 成功）。内核侧不合成逐条样本。
- `xtier_migrate_range` 是 xTier 私有内核补丁；不改内核就没有等价入口。

因此本实现整体落在用户态。xTier 自带 `XTIER_USERSPACE_POLICY=1` 用户态孪生
（`page_profiler_user.c` 的 `us_*` 段：同一 INT8 MLP、同一滤波器、同一 top-K 门），
本项目的策略面就是那段代码的移植——**策略语义逐字节一致**，搬动的只有样本源和执行器。

## 映射表（xTier → xtier-arm）

| xTier 原实现 | xtier-arm | 依据/验证 |
|---|---|---|
| PEBS `MEM_UOPS_RETIRED.ALL_LOADS`，freq=2000/s | SPE per-task 事件，`load_filter=1`，`sample_period`=间隔（默认 4096 ops，环境变量 `XTIER_SPE_INTERVAL`） | 机器上有 `arm_spe_0`（type=19），min_interval=1024；per-task open 成功 |
| BPF `on_pebs_stage0` 逐样本状态更新 | 用户态 `sample_cb`（`us_sample_cb` 移植），aux 逐记录解码 | 记录格式实测：48B/样本（b0+8B 指令VA、b2+8B 数据VA、62+4B events、49+1B op、65+4B context=tid、71+8B 时间戳） |
| 内核 BPF 推理（tail-call MLP） | 用户态 INT8 MLP（`us_mlp_forward` 原样） | 与 xTier 用户态孪生相同 |
| `xtier_migrate_range` 促升 (→node0) | 内核需求换页：访问已换出页自动 major fault 换入 | 见下"方向翻转" |
| 内核模块迁移 (→node1) + PTE A-bit 冷扫描降级 | `process_madvise(pidfd, MADV_PAGEOUT)` + `page_idle` 位图冷扫描 | 实测 64MB MADV_PAGEOUT → present=0 swap=1；page_idle 存在；MADV_PAGEOUT 在 5.10 process_madvise 白名单内 |
| `move_pages` 1Hz tier 回读 | `/proc/pid/pagemap` present/swap 位回读 | 实测 pagemap 位正确反映换出/换入 |
| node0 容量 = offline 内存块 | 预算 = RSS 目标，由 daemon 降级到预算线（无 cgroup 限制，内核不自主回收） | — |

## 方向翻转（本移植最重要的语义决策）

xTier：内存**分配在慢层**（CXL），ML 预测"促升收益"，把热页搬上 DRAM；降级由
PTE 冷扫描兜底。

DRAM+SSD 上方向必然倒转：内存**分配在快层**（DRAM），ML 分数语义不变
（"未来 10 epoch 的再访问收益"），应用面反转：

- **降级是主决策**：预算压力下按分数升序把低分常驻页 `MADV_PAGEOUT` 到 SSD。
  分数高的页=模型预测会被再访问=留在 DRAM。促升侧不需要 ML：换出页被访问时
  内核自动 major fault 换入（=隐式促升，成本即时可见）。
- **换出页无法被采样**（SPE 只采驻留地址），所以"主动促升"没有增量信号——
  预测性促升的正确位置就是降级决策本身（别把即将变热的页换出去）。
  这是介质属性决定的（CXL 页驻留可采、swap 页不存在于地址空间），
  不是实现捷径。冷却期（15 epoch）防换出-回fault抖动。
- top-K 门：`histogram → cutoff → thresh` 数学逐行移植（`topk_threshold_from_hist`），
  语义变为"DRAM 保留 top-K"。

## 已知偏差（诚实清单）

1. 策略在用户态：xTier 用户态孪生即此设计；开销进 daemon（`[EPOCH] proc_ms` 可见）。
2. 降级/换回成本不对称于 CXL 迁移：MADV_PAGEOUT 写回 SSD（顺序 ~129MB/s 写），
   回 fault 读 4K qd1 ~150µs。实验规模须按此缩放（见 README-实验）。
3. 单线程 daemon 做推理+扫描+迁移；288 核机器上占比极小（`proc_ms` 上报）。
4. 每个 CPU 只能有一个活动 SPE 事件：目标线程间共享核心会 PMU 多路复用丢样本；
   负载绑定 CPU 时无此问题。
5. fp 指纹特征（CSV 的 fp*_hex 列）在用户态同样容易实现但训练脚本不用它，
   固定输出 0。
6. 修了 xTier 上游的一处潜在 UB：`check_heuristic_filter` 的冷检测循环
   `i < COLD_EPOCHS-1`（=9）越界读 `hits_prev[6..8]`（只有 6 项）；
   本移植夹紧到 `HIST_STEPS`。行为差异：冷检测实际只看 6 个历史 epoch。

## 结构

```
src/xtierd.c   主循环：epoch 驱动、模式（monitor/collect/evaluate/run）
src/spe.c      SPE 采样器：per-task 事件 + aux 解码（GPL，参照 perf arm-spe 解码器）
src/policy.c   状态表/特征/INT8 MLP/滤波/top-K（us_* 移植）
src/exec.c     执行器：MADV_PAGEOUT、pagemap 回读、page_idle 冷扫描、预算
ml/            训练管线（train_and_export.py 从 xTier 原样复制，Apache-2.0）
bench/         zipf_chase / phase_zipf 负载（自 xTier 移植）+ 运行脚本
env.sh         机器准备（sysctl/swap/THP）
```

## 用法

```sh
# 机器准备（root）
source env.sh && xtier_arm_sysprep

# 1. bootstrap 模型 + 编译
python3 ml/bootstrap_model.py ml/models/mlp_q8_bootstrap.h
make MODEL_HDR=$PWD/ml/models/mlp_q8_bootstrap.h

# 2. 采集（也可以直接跑 run 模式用 bootstrap 权重先看管线通不通）
numactl --cpunodebind=0 --membind=0 bench/zipf_chase 8 0.9 50 32 1 &
sudo src/xtierd $! 4096 250 collect     # -> features.csv

# 3. 训练（任意有 torch 的机器；CSV 拷过去）
python3 ml/train_and_export.py          # -> ml/models/mlp_q8.h

# 4. 运行
make MODEL_HDR=$PWD/ml/models/mlp_q8.h
sudo XTIER_DRAM_BUDGET_MB=6000 src/xtierd <pid> 4096 250 run
```

环境变量：`XTIER_SPE_INTERVAL`（默认 4096 ops）、`XTIER_SPE_STORES=1`、
`XTIER_DRAM_BUDGET_MB`、`XTIER_TOPK_FRACTION`（默认 0.15）、
`XTIER_COOLDOWN_EPOCHS`（默认 15）、`XTIER_MIG_BUDGET`（每 epoch 最大迁移页数，
默认 2048）、`XTIER_COLD_SCAN_CHUNK`（默认 50000）、`XTIER_STATE_BITS`（默认 22）、
`XTIER_FEATURES_CSV` / `XTIER_STATS_CSV`。

## 训练目标（与 xTier 相同）

```
reuse_intensity = 该页未来 10 epoch 的命中数
tenure          = 其中非零 epoch 数
reuse_speed     = 距首次再访问的 epoch 数（≤10）
benefit         = reuse_intensity * (tenure/10) / (1 + reuse_speed)
```

在本移植里 benefit 的含义 = "把该页留在 DRAM 的价值"（换出后会很快回 fault）。
CSV schema 与 xTier 逐列一致，`train_and_export.py` 无改动。


## 首次实验记录（2026-11-23，192.168.90.7）

### 环境与负载
zipf_chase，3 GB 匿名内存、2 MB 块、θ=0.9、8 线程（绑 node0），epoch=1 s，
SPE interval=4096 ops（load only），预算由 `XTIER_DRAM_BUDGET_MB` 控制。

### 管线验证
- 采样：~1.6–2.4 M 样本/150 s（~13 K 页/epoch 峰值），48 B/样本；
- 推理：~30 µs/页（单线程，24.7K MACs，-O3）；proc_ms ≈ 150 ms/10K 页；
- 降级：`process_madvise(MADV_PAGEOUT)`，1024-iovec 批，demote_fail=0；
- 回读：pagemap present/swap 位；冷扫描：page_idle 全地址空间分片（50K PTE/次）。

### scorer 消融（exp3：3 GB 集 / 2.5 GB 预算 / 120 s，每配置一次）
| scorer | demote_ok(尝试页) | majflt | majflt/s | 每降级页回 fault 率 |
|---|---|---|---|---|
| mlp（训练模型） | 400,973 | 131,065 | **1111** | **0.33** |
| freq（命中数排序） | 831,537 | 365,718 | 3099 | 0.44 |
| random | 600,136 | 235,837 | 2225 | 0.39 |

MLP 在更少的降级量下取得最少的回 fault：每次降级的回 fault 率 0.33 < random 0.39
< freq 0.44——排序质量的作用与 xTier 原文的 scorer 消融签名一致。
置信度：单负载、单预算、单次运行；demote_ok 统计的是"提交给内核的页数"
（MADV_PAGEOUT 返回值不区分是否真正换出），与 xTier 的 att/ok 分离同性质。

### 已知动态（诚实记录）
1. **驻留页数不收敛到预算线**（3.17 GB vs 2.5 GB 预算）：降级候选池每 epoch 只覆盖
   被采样过的页；从未被采样的页只能靠 page_idle 冷扫描（1024 页/epoch 上限），
   所以系统稳定在"边际换出-换回 churn"平衡态，而不是把驻留压到预算。
   要压到预算线需把冷扫描产出也接入预算驱动（后续工作）。
2. **回 fault 受 SSD qd1 上限约束**：pswpin 峰值 ~4700 页/s ≈ 6700 IOPS；
   这是 3 GB/2 GB 这类激进预算下必现的介质物理。
3. top-K 阈值随分布动态变化（72→116→12），门在起作用；
   keep 集（每 epoch ~1K 页，top ~10%）即 DRAM 保留集。
4. 换出-换回是主要成本通道：demote(写 SSD) ~16 MB/s 上限受 epoch 预算控制，
   fault-back(读 SSD) 受 qd1 延迟限制。

### 后续工作
- 把冷扫描产出纳入预算驱动，使驻留真正收敛到预算线；
- XTIER_SCORER=recency/ewma 补全消融；
- phase_zipf 相位变化实验（对应 xTier figures/plot_phase_response）；
- 与内核 LRU（高 swappiness、无 ML）基线的 A/B：同一负载、无 xtierd 对照组。


### MLP vs 内核 LRU 降级对照（exp5，2026-11-23）

设计：同负载（zipf_chase 3GB/θ0.9/8 线程/300Mops）、同等效 DRAM 预算 2.5GB、
swappiness=100。
- `lru` 基线：cgroup v1 `memory.limit_in_bytes=2.5GB` 硬限，内核 LRU 挑受害者，无 daemon；
- `mlp`：cgroup 2.9GB 留余量，daemon 按 MLP 排序主动降级到 2.5GB（exp4 教训：
  硬限先于 daemon 触发时 demote_ok=0，daemon 必须有 headroom 才能抢在内核前选人）。

| | majflt 总量 | majflt/s | VmSwap 终值 |
|---|---|---|---|
| 内核 LRU（cgroup 2.5GB） | 1,511,160 | 11,621 | 722 MB |
| **MLP daemon（2.5GB 预算）** | **635,849** | **~4,890** | **377 MB** |

**回 fault 降 ~2.4×，换出量减 ~1.9×。**

诚实脚注：mlp 组的实际驻留 ~2.82GB（内核仍按 2.9GB 上限参与了回收，
daemon 实际只降了 9 万页），即受害者选择是 MLP+内核混合，2.4× 是混合策略
对纯 LRU 的下界；两组 Trial 都未在 130s 内跑完 300 Mops（thrash 区），吞吐对比
需更长运行窗口。另 exp4 第一轮（无 headroom）暴露的"cgroup 硬限先触发则
daemon 失效"是部署该方案时的必要注意事项。


### exp7–exp10（吞吐对照与修正，2026-11-23 深夜）
- 三补丁（fault 钉住/资格门槛/风暴滞后熄火 16K→13.6K）+ 单驱逐者架构（cgroup 3.2GB
  headroom）+ ops 探针（修掉探针假共享 bug）。
- 最终同内存对照：LRU@3.2GB 66.8s vs MLP 77.6s（daemon ~16% 纯开销）；exp9 的 20×
  是内存混淆（MLP 组驻留 3.19GB vs LRU 2.5GB）。预算执行仅 65MB/63epoch——候选
  发现通道（page_idle 逐页查询 + 2K samples/epoch）是瓶颈，不是模型。
- 详见根目录《xtier-arm-复现报告.md》补充一节。
