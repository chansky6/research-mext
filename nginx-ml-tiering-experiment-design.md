<!-- SPDX-License-Identifier: Apache-2.0 -->
# nginx 真实负载 ML 分层效果评估实验方案

日期：2026-11-18。前置成果：`docs/SW-SAMPLING-PORT.md`（软件采样管线）+ trapgraded3 对照实验（ML 优于随机已证明）。

---

## 1. 研究问题与假设

**RQ1（决策质量）**：ML scorer 在真实访问模式下，是否比基线（random/freq/无分层）把更多 DRAM 名额花在真实热数据上？

**RQ2（性能收益）**：在内存紧张配置下，ML 配置是否用更少的 DRAM 达到同等性能 SLO？（"满足性能阈值的最低内存配置"）

**RQ3（运行成本）**：ML 管线的 CPU/内存开销是否在可接受范围（≤1 核 5%，与 xTier 论文自报口径对齐）？

**假设**：
- H1：ML 的晋升质量（热质量命中比）显著高于 random/freq（trapgraded3 已给出微基准证据，本实验验证真实负载下是否成立）
- H2：DRAM 预算受限时，ML 配置的最低达标内存配置 ≤ random 基线（预期差距随预算收紧扩大）
- H0：无显著差异 → 报告负结果

## 2. 关键前提与拓扑选型

### 2.1 为什么不是裸 nginx 静态站

nginx 静态文件服务的热数据在**内核 page cache**，经 `sendfile` 直接送网卡，**不经过任何进程的用户态 PTE**。当前管线（executor A-bit 扫描器）只遍历目标进程 `mm` 的页表 → **看不见 page cache 页**。裸 nginx 实验会把"要分层的对象"漏掉，只剩 nginx 进程自身的少量匿名内存（连接缓冲等），实验退化。

### 2.2 采用拓扑：nginx 前置 + memcached 数据面

```
wrk (负载发生器, node1 vCPU)
   │  HTTP GET /key/N
   ▼
nginx (worker 绑 node0 vCPU, memcached_pass)
   │  文本协议 get
   ▼
memcached (数据面, --preferred=1 全量分配在慢层)
   │  匿名内存 = 被分层对象 ← xTier 瞄准这个 PID
   ▼
xTier: A-bit 扫描 → MLP 打分 → promote/demote (node1 ↔ node0)
```

- 全部是开箱即用的真实软件：nginx 原生 `ngx_http_memcached_module`（只读 GET 路径，正好匹配读密集基准）；memcached 是 tiering 文献的标准被测对象（TMO/Memtis 均用它）
- 数据面（memcached）= 纯匿名内存 + 按 key 访问 → xTier 管线可见、可用 census 工具核查页位置
- nginx 层保留用户要求的"真实前端"，同时其自身匿名内存可作为次要观察对象

### 2.3 两个实验家族（诚实划界）

| | 家族 A：决策质量 + 成本 | 家族 B：性能与最低内存配置 |
|---|---|---|
| 可运行环境 | **当前 QEMU TCG 即可** | **需真机**（Ice Lake+ / KVM / PEBS 或至少快慢层有真实延迟差） |
| 原因 | 页位置判定（census/move_pages）不依赖层间速度 | QEMU 中慢层经宿主 page cache 与 DRAM 同速，吞吐/延迟差异为零，性能数字必然无效 |
| 核心指标 | 晋升质量、名额利用率、迁移流量、乒乓率、CPU 开销 | 吞吐、P50/P95/P99、最低达标预算 |

**当前环境执行家族 A；家族 B 方案完整给出、资产复用（脚本/拓扑/训练流程全部同构），真机到手即跑。**

## 3. 实验变量

### 3.1 自变量（independent）

| 变量 | 取值 |
|---|---|
| **Scorer 配置** | `mlp`（ML）/ `random`（消融对照）/ `freq`（频率启发式）/ `none`（不加载 xTier = 无分层基线） |
| **DRAM 预算**（node0 上限） | 工作集的 ×{1.0, 0.5, 0.25, 0.125}（家族 B 主变量；家族 A 固定 0.25 一档即可） |
| **负载场景** | S1/S2/S3/S4（见 §4） |

### 3.2 控制变量（每次运行必须恒定）

| 项 | 固定值/方法 |
|---|---|
| 内核与模块 | 6.11.0-xtier 同一 bzImage；`.ko` 同一构建（工作区当前版）；`touch_max_per_scan=65536`、`worker_interval_ms=10` |
| insmod 参数 | **collect 阶段 `enable_migration=0`，run 阶段 `=1`**（traptest.sh 踩过的坑，run/collect 必须分开） |
| memcached | 同版本；同 `-m <store大小>`；同并发上限；预热后的 key 集与 value 大小完全一致（固定种子生成） |
| nginx | 同版本、worker_processes 固定、`memcached_pass` 同上游、keepalive 同配置、sendfile/缓冲参数锁定 |
| 负载 | 固定 RPS（开环恒速，`wrk` 固定连接数+固定速率；避免闭环饱和混入 CPU 瓶颈）、固定种子、固定请求分布参数（Zipf s） |
| 亲和性 | wrk → node1 vCPU；nginx worker → node0 vCPU；memcached → node0 vCPU（计算在快层）、`--preferred=1` 首触在慢层；profiler loader 按 xTier 默认钉 node1 |
| 系统状态 | THP=never、swapoff、`drop_caches` 只在装料前、run 前 rmmod+清 pin+重 insmod+**重装数据**（页位置是物理状态，见 graded 残留污染教训） |
| 时钟类 | epoch_ms=1000（memcached 相变周期分钟级，无需 100ms；也降低 loader 泵成本） |

### 3.3 混杂变量与消减措施

| 混杂源 | 消减 |
|---|---|
| 运行顺序漂移（TCG 宿主热噪声/缓存） | **顺序交错**：ML,random,ML,random… 每对相邻；每 rep 重排 |
| 上一 run 的物理残留 | 每 run 之间全量重置（rmmod → rm pins → insmod → 重启 memcached → 重新装料 → 预热） |
| 背景触碰噪声（~20 行/页/300s，colddetect 实测） | 热区判定阈值取"显著高于噪声底"（预期热质量占比 ≥25% 时不受影响）；决策质量指标用比例而非绝对计数 |
| 循环/计时类错误（v1 教训：loop 实际 29ms） | 所有周期类参数以墙钟验证：装料后先跑 3 分钟烟测，从特征 CSV 抽查触碰间隔分布是否符合设计 |
| 管线静默失效（enable_migration 教训） | 每次 run 后强制检查：`enable_migration=Y`、`nr_promote_att>0`、touch 扫描 dmesg 在涨；任何为零 → 该 run 作废重跑 |

## 4. 负载场景矩阵

数据集：memcached 存储 1.5 GiB，键空间 100k，value 大小混合（64KB 为主 + 少量 256KB），键→value 由固定种子生成。选 Zipf s 使**预期热质量（累计访问概率覆盖的 value 字节）≈ 工作集的 25-35%**（pilot 时从发生器日志实测校准）。

| 场景 | 访问模式 | 考察点 |
|---|---|---|
| **S1 稳态 Zipf** | 全程同一 Zipf(校准 s) 分布，恒 RPS | 基础判别力；热集稳定下的收敛速度 |
| **S2 相变轮转** | 热集每 120s 轮换一个子集（key 空间分 8 组，热组循环） | 适应性：相变后热集重建速度（xTier 论文主打能力） |
| **S3 陷阱混合** | Zipf 主体 + 10% 均匀随机长尾 | 真实版 trapgraded3：候选流被冷页污染时 ML 的抗性 |
| **S4 突发** | RPS 每 60s 在 ×1 与 ×2 间阶跃 | 压力下的稳定性（乒乓率、队列积压） |

RPS 选取原则（家族 B 真机）：先在全 DRAM 无分层配置下扫描，取 **CPU 利用率 ≤75%** 的最高 RPS 为标定点（避免测到 CPU 瓶颈而非内存效应）。

主实验矩阵建议分两档：
- **Stage 1（最小可信）**：S1 + S2 × {mlp, random, none} × 预算 {0.5, 0.25} × 3 reps = 36 runs
- **Stage 2（扩展）**：加 S3/S4、freq 配置、预算 4 档、5 reps

## 5. 数据采集与字段字典

### 5.1 采集源

| 源 | 方法 | 频率 |
|---|---|---|
| wrk | `wrk -R <rps> --latency`，JSON 化输出（wrk 自带 Lua 脚本或解析文本） | 每 run 一次汇总 + 10s 粒度延迟轨迹 |
| memcached | `stats` 差分（cmd_get/hit/miss/evict）、`lru_crawler metadump` 取 item 级 last_access（item 级"真实热度"证据） | run 前后各一次 metadump；stats 每 30s |
| xTier | BPF `stats_map` dump（promote/demote att/succ、ml pos/neg、queue_full、thrash、sw_touches） | run 结束时全量 |
| census | 适配版 census 工具：扫 `/proc/$memcached_pid/maps` 全部大块 rw 匿名区，`move_pages` 查询模式逐页统计 node 分布（**工具需小改**：从单 buffer 扩为多 VMA 遍历） | baseline + 终态（相变场景加中间点） |
| dmesg | touch 扫描 calls/touches 序列 | 健康检查 |
| CPU/内存 | `pidstat -t -p <loader>,<memcached>,<nginx workers>` 10s 粒度；`grep cpu /proc/$(pgrep xtier_worker)/stat` 差分 | 全程 |

### 5.2 数据字段（每 run 一行记录）

```
run_id, date, family(A|B), stage, scenario(S1-4), scorer(mlp|random|freq|none),
dram_budget_mb, rps_target, zipf_s, seed,
vm_reset_done(Y/N), enable_migration_verified(Y/N),
---- 负载结果 ----
throughput_rps_mean, throughput_rps_std, latency_p50_us, latency_p95_us,
latency_p99_us, latency_max_us, errors, timeouts, non2xx,
---- memcached ----
mc_get_delta, mc_hit_rate, mc_evictions_delta,
---- 分层行为 ----
sw_touches_total, inferences_total, promote_att, promote_succ, promote_fail,
demote_att, demote_succ, tier_flips, thrash_1s_events, queue_full_events,
ml_score_pos, ml_score_neg, ml_score_zero,
---- 页位置 ----
census_node0_pages, census_node0_bytes, census_total_pages,
(家族B另加) census_by_expected_hot_class: node0_hotshare, node0_coldshare,
---- 成本 ----
loader_cpu_s, module_worker_cpu_s, loader_rss_mb, bpf_maps_mb,
---- 判定 ----
slo_pass(Y/N), run_valid(Y/N), abort_reason
```

### 5.3 ground truth（真实负载的关键难点）

真实负载**拿不到页级标签**（memcached 不暴露 item→VA 映射）。三级替代：

1. **质量代理（主用，家族 A/B 通用）**：metadump 的 item 级 last_access + 发生器请求日志 → 预期热质量（expected hot bytes）；对照 census 的 node0 实际占用。**指标：名额有效利用率 = node0 上"预期热质量覆盖的字节" / node0 总字节**——需要 item→近似页区间的映射假设（slab 分配器按 size class 连续装页，同 class 热页近似成簇；误差声明为系统偏差）
2. **结果导向（家族 B 主判定）**：不做页级归因，直接用 SLO-最低预算曲线判胜负（见 §6）
3. **微基准对照锚点**：trapgraded3 已给出页级精确证据，本实验的页级空白由它补强（文档引用关系）

## 6. 最低内存配置评估法（家族 B 核心）

**SLO 定义（pilot 阶段预注册，之后不改）**：
- 先跑"全 DRAM 无分层"（`none` + 预算=100%）得到基线 p99_baseline 与吞吐 baseline
- SLO：`p99 ≤ 1.10 × p99_baseline` 且 `吞吐 ≥ 0.95 × baseline` 且 `错误率 < 0.1%`

**扫描协议**（每个 scorer 配置独立执行）：
1. 预算档位：{100%, 50%, 25%, 12.5%, 6.25%} × 工作集（粗扫）
2. 在"达标↔不达标"的相邻档间做二分细化（≤3 步）→ **DRAM_min(scorer)**
3. 每 budget 档 ≥3 reps，取"≥2/3 reps 达标"才判该档达标（SLO 的一致性判定）

**报告口径**：
- `DRAM_min`（MB）与节省比 `1 - DRAM_min/WS`
- 主结论：`DRAM_min(mlp)` vs `DRAM_min(random)` vs `DRAM_min(freq)`，配 CI
- 等价表述："在 SLO 下，ML 比随机少用 X% DRAM"——这就是"满足性能阈值的最低内存配置"的直接答案

## 7. 实验步骤

### 阶段 0：资产准备（一次性，约半天）
1. guest 装 nginx（`memcached_pass` 可用）+ memcached + wrk（Lima 交叉编译或 guest apt）
2. 写数据装料器（固定种子生成 key/value；可选 Python `memcache` 客户端或 memaslap）
3. 写发生器封装（wrk Lua 脚本：S1-S4 的 key 分布函数 + 固定 RPS 开环）
4. census 工具适配（多 VMA 版，见 §5.1）
5. 写 `run-nginx.sh`（由 traptest.sh 改造）：stage-aware insmod（collect=0/run=1）、全量重置、健康检查断言（enable_migration、promote_att>0、touch 在涨）、自动字段采集与 CSV 追加

### 阶段 1：Pilot（每次环境变更后重做，约 1 小时）
1. 装料 → `none` 配置跑 5 分钟 → 定 RPS 标定点与 p99_baseline → **预注册 SLO**
2. collect 10 分钟（S1）→ 检查特征 CSV：memcached 大块匿名区被扫到、触碰间隔分布与 Zipf 预期一致（墙钟校验，§3.3）
3. 计算预期热质量占比，必要时调 Zipf s

### 阶段 2：训练（每场景一次，~15 分钟/run）
- 主方案：**S1 采训 + S1/S2/S3 评估**（考察泛化与相变适应，避免"训什么测什么"的过拟合质疑）
- 对照方案（论文口径）：每场景独立采集训练（xTier 论文就是 per-workload 训练）
- Lima 训练（二值化特征补丁版 train_and_export.py）→ DIAG 检查（val loss 下降、promote 区占比与预期热占比同量级、输出不塌缩）→ 失败即 NO-GO 重查采集

### 阶段 3：主实验
1. 按 Stage 1 矩阵生成 run 清单，**随机化+交错顺序**执行
2. 每 run：全量重置 → 装料 → 预热 5 分钟（不计数据）→ 采集窗口 10-15 分钟 → 终态 census → 字段落 CSV
3. 健康检查失败的 run 立即作废重跑，记录 abort_reason
4. 每晚（或每 12 runs）做一次中间 sanity：分组均值点图，发现漂移立即停

### 阶段 4：家族 B（真机）
- 拓扑/脚本/训练资产整体迁移；KVM + `-cpu host` + PEBS（SPEBS 采样恢复原版语义，特征管线不变）
- 增跑 TPP/AutoNUMA 基线（BASELINES.md 方案，QEMU 中已证不可跑）
- 上述 SLO-budget 扫描

## 8. 结果表格模板

**表 1：主结果（家族 B）——SLO 达标最低预算**

| Scorer | S1 DRAM_min(MB) | S1 节省% | S2 DRAM_min | S2 节省% | S3 DRAM_min | S3 节省% |
|---|---|---|---|---|---|---|
| none（无分层,全DRAM） | — | 0（参照） | | | | |
| freq | | | | | | |
| random | | | | | | |
| **mlp** | | | | | | |

（每格 = 该档 ≥2/3 reps 达标的最低预算；附 95% CI）

**表 2：等预算性能对比（家族 B，固定预算 25% WS）**

| Scenerio | Scorer | RPS | p50(µs) | p95(µs) | p99(µs) | SLO达标 | hit_rate |
|---|---|---|---|---|---|---|---|
| S1 | mlp | | | | | | |
| S1 | random | | | | | | |
| ...（mean±std over ≥3 reps） |

**表 3：决策质量（家族 A/B 通用）**

| Scorer | 名额有效利用率% | promote_succ | 误晋升规模(推断) | tier_flips(乒乓) | 热集重建时间(S2 相变后, min) |
|---|---|---|---|---|---|
| mlp | | | | | |
| random | | | | | |
| freq | | | | | |

**表 4：运行成本（RQ3）**

| 配置 | loader CPU(核·s/run) | 模块 worker CPU | BPF maps 常驻(MB) | 对吞吐的拖累% |
|---|---|---|---|---|

**表 5：run 台账（原始记录，每 run 一行）**：§5.2 全字段。

## 9. 统计与结论判定

**可靠性判定**：
- 每格 ≥3（目标 5）reps；报告 mean±std
- 显著性：同 budget 同场景下 scorer 两两配对，配对 t 检验（正态时）或 Wilcoxon 符号秩（n≥5）；多重比较用 Holm 校正
- 效应量：Cohen's d + 差值的 95% CI；**同时报告实际显著性**（pp 差值）——统计显著但 <3pp 的差异按无实际意义报告
- SLO 类离散判定：报 k/n 达标率，不做百分比平均

**结论方法（预注册，防事后挑选）**：
1. **主结论 = 表 1**：`DRAM_min(mlp) < min(DRAM_min(random), DRAM_min(freq))` 且节省差 ≥10%（MEXT 商业叙事的门槛值）→ H2 成立；否则如实报告"未达预注册门槛"
2. **次结论 = 表 3**：ML 的名额有效利用率显著高于 random（对应 trapgraded3 的 74.2% vs 37.2%，观察真实负载下是否保持量级）→ H1
3. **成本结论 = 表 4**：开销 ≤ 1 核 5% → H3，否则标 red flag
4. **负结果处理**：任一假设失败时，给出机制层归因（用表 5 台账 + dmesg + 特征抽查定位：采样保真不足 / 模型泛化失败 / 执行吞吐瓶颈），不做超过数据的推断
5. 与既有结论链的关系：本实验 = trapgraded3（页级精确、人造负载）→ 真实负载（item 级近似、真实软件）→ 真机（性能数字）。三层互为证据链

## 10. 已知风险与本环境教训对照

| 风险 | 来源教训 | 本方案的防线 |
|---|---|---|
| 管线静默失效 | traptest.sh enable_migration=0 | run 后健康断言（§7 阶段 3 步骤 3） |
| 周期估算错误 | trapgraded v1/v2 loop 29ms 教训 | pilot 特征抽查触碰间隔分布 |
| 物理状态残留 | graded 随机组污染 | 每 run 全量重置+重装料 |
| 背景触碰噪声 | colddetect 实测 ~20 行/页/300s | 热质量占比校准 ≥25% |
| QEMU 性能数字无效 | 快慢层同速（QEMU-REPRO 已证） | 家族 A/B 划界，不混报 |
| 训练塌缩 | graded v1（val loss 恒 133） | DIAG 检查为 go/no-go 门槛 |

## 11. 工作量估计

| 项 | 当前 QEMU（家族 A） | 真机（家族 B） |
|---|---|---|
| 阶段 0 资产 | 半天 | +2 小时（KVM 化 + 基线开关） |
| Pilot | 1 小时 | 1 小时 |
| Stage 1 矩阵 | 36 runs × ~12min ≈ 7.5 小时（可过夜跑） | 同规模 ~7.5 小时 |
| Stage 2 扩展 | 视 Stage 1 结果 | SLO-budget 二分扫描 +3-5 小时 |
