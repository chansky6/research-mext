# Equilibria 论文总结

**Equilibria: Fair Multi-Tenant CXL Memory Tiering At Scale**
Kaiyang Zhao、Neha Gholkar、Hasan Al Maruf、Abhishek Dhanotia、Johannes Weiner、Gregory Price、Ning Sun、Bhavya Dwivedi、Stuart Clark、Dimitrios Skarlatos
Carnegie Mellon University、Meta
arXiv:2602.08800v2 [cs.OS]，2026-04-20（预印本，PDF 内无会议信息）

## 一句话

给 CXL 分层加上**多租户公平**：每个容器两个旋钮（`lower protection` 保底 / `upper bound` 封顶）+ 按容器计的分层可观测性 + thrashing 抑制，从而在共置场景下分别达到各自 SLO——相比 Linux 上游（源自 TPP）的方案，生产负载最高 **+52%**、基准 **1.7×**。

## 问题

论文把多租户分层失效归为四条**可观测路径**：

1. **系统级最热页优先**：只按"全机最热"决策，冷访问模式的租户被整体挤出本地内存（Fig.3）。
2. **启动顺序决定命运**：先到者占满本地内存，晚到 30 s 的容器吞吐损失 **−28%**。
3. **thrashing 污染**：某个在两 tier 间抖动的负载占用迁移带宽与 demotion/promotion 处理能力，邻居的迁移活动被压到原来的 **1/10 以下**，吞吐 −7%。
4. **突发分配的连带损伤**：邻居突发分配触发同步 demotion、NUMA hint fault 增多，直接抬高 tail latency。

此外还有三个结构性缺口：控制面只有"系统级最热"或"critical/best-effort"粗分类；demotion/promotion 与 hint fault 的设置与触发**未按租户虚拟化**（运维无法按容器诊断）；对 thrashing 完全不感知。

## 机制

**① 公平性的定义（p5）**

> "Fairness is defined as each colocated workload being able to run at a performance level that meets its SLO, regardless of when it is launched and with whom it is colocated."

实现上落到**内存放置**，代理量是"本地内存的公平份额"——"保证一定比例的最热页在快速本地 DRAM 中即已足够"。理由：CXL 带宽对容量受限型负载不构成争用，故不额外建模带宽公平。

论文同时挑明：*"The fair share of memory is subjective and depends on workload characteristics and SLOs."* 所以份额需要按负载离线 profiling（Web 用 10 分钟热点足迹，CI 用容量比折算）。

**② 两个 per-container 旋钮（p5）**

| 旋钮 | 语义 |
|---|---|
| `lower protection` | 本地内存的**受保护量**：邻居竞争时不被 demote/reclaim；**work-conserving**，空闲时可超出自己的份额 |
| `upper bound` | 本地内存的**硬上限**：超出即 demote |

二者通过 cgroup 接口设置与导出。对比 Memtis 只能设"快层用量上限"，Equilibria 同时提供 work-conserving 的保底。

**③ 分层可观测（p6）**：cgroup 结构里按 tier 增加 local/CXL 用量计数器，以及 demotions、promotions、attempted promotions 等活动计数器。

**④ 降级调节（p6）**：本地用满后按"超出 lower protection 的比例"定优先级，扫描量

$$d_{scan} = n_{lru}\,\frac{n_{cgroup} - n_{protection}}{n_{cgroup}}$$

低于 protection 的容器**免于 demotion**；upper bound 由后台线程温和 demote，分配路径上同步 demote 而**不 OOM kill**。

**⑤ 提升调节（p7）**：超份额容器标记为 promotion-throttled，扫描量

$$p_{scan} = p_{base}\cdot\min\left(\left(\frac{n_{protection}}{n_{cgroup}}\right)^4,\ \frac{1}{16}\right)$$

四次方使"超 1% 时仍扫 96%、超 10% 时降到 68%"；下限 1/16 保证 CXL 里极热的页仍能被拉升。

**⑥ thrashing 抑制（p7）**：用固定大小哈希表采样记录最近提升页的 PFN + 时间戳；demote 时若驻留时间 < $t_{resident}$ 计一次 thrashing。后台每 **5 s** 统计各容器 thrashing 速率，超过 $r_{thrashing}$ 则下周期晋升速率**减半**，低于则**翻倍**恢复。仅对处于 steady state 的容器生效（用 active page 数与 page freeing 速率两个指标判定，阈值来自 5000 台服务器上 5 个主要服务的采样分析）。

**⑦ 实现（p7）**：Linux 6.11，约 **1,000 行**，改动限于 cgroup、demotion、promotion 代码；补丁已提交社区、部分已 upstream。

## 评估设置

**硬件**：Meta 单路 AMD Turin。large：**768 GB DDR5 + 256 GB CXL DDR4（3:1）**，CXL 空载延迟 **252 ns**；small：256 GB + 64 GB。CXL 为 2.0 Type 3 扩展器，介质是退役回收的 DDR4；交换关闭。

**基线**：**只有一个**——Linux 上游的内存分层支持（即源自 TPP）。Memtis 仅定性讨论；**全文未引用 TMO**（尽管 TMO 一作 Weiner 同为本文作者）。

**负载**：Meta 生产负载 Cache（回放生产 trace）、Web（真实流量，5 实例/机）、CI（构建测试，4 容器/机）；DCPerf 的 SparkBench + TaoBench（1×SparkBench + 3×TaoBench）；自制顺序 pass 微基准。**三者都是容量受限型、CXL 带宽利用率 <10%**。

## 关键数字

| 指标 | 数值 | 对比 |
|---|---|---|
| 端到端 | 最高 **+52%** | Linux/TPP（生产负载） |
| 吞吐 | **1.7×**（SparkBench 11.2 vs 4.2 QPS） | Linux/TPP |
| 无公平机制时的损失 | Cache 吞吐 **−65%**；失败请求 340/min → **83K/min** | 两实例同机 |
| 迟到容器 | 吞吐 **−28%** | 同热度、晚启 30 s |
| thrashing 干扰 | 邻居迁移量降至 **<1/10**、吞吐 −7%；抑制后邻居 290K→**310K pages/s（+7%）** | 基线 Linux |
| 超份额容器的降级速率 | 达 **250K pages/s** | 份额 150% 的容器 |
| 稳定态 local 占比 | Equilibria 72%/73%；Linux 90%/70% | 两实例 Cache |
| P99 延迟 | A 降 **52%**、B 降 **47%**；B 仅比 A 高 **7%** | Linux（NUMA hint fault 减少 97%） |
| Web | partition A 36 RPS / B 80 RPS（Linux 74 RPS，**+8%**） | SLO：相对独占本地劣化 <2% |
| CI | 平均构建时间 **−3.3%**；容器 D 起步即 >90% 在本地（Linux >50% 落 CXL） | Linux |
| 硬件背景 | CXL 已承担服务器总内存 **20–30%**；内存占机架 TCO 44%/功耗 24%（Year 11） | Meta 现状 |
| 真实 vs 模拟 CXL 延迟 | 空载高 **27 ns**、30% 负载下高 **37 ns** | 关核模拟 |
| CXL:local 带宽 | 30 GB/s vs 300 GB/s（0.1） | Gen1 |

## 论文自陈的局限

论文**没有独立的 Limitations 章节**，但明确写出了以下几处：

1. **可观测性只做定性论证**：*"we do not provide a quantitative evaluation of the container-level observability"*（p8）。
2. **公平份额是主观的**：依赖负载特征与 SLO，需要离线 profiling 每个负载的热比例（p5）。
3. **抑制晋升牺牲收敛速度**：保留 1/16 的基础扫描率即为"用布局收敛速度换应用性能"（p7）。
4. **thrashing 两个阈值（$t_{resident}$、$r_{thrashing}$）未给具体数值**（p7）。
5. **纯软件迁移/hotness 开销**：期望未来有面向页迁移与热度跟踪的硬件支持（p4）。
6. **只覆盖容量扩展场景**：三个生产负载都是容量受限、CXL 带宽利用 <10%，带宽受限场景未覆盖（p4、p9）。
7. **可部署性依赖 cgroup 路径**：*"most prior work, except for TPP, is not upstreamed to Linux"*（p11）。

## 我的判断

**它把"公平"从一个形容词变成两个可执行的旋钮。** 这是全文最实用的贡献：`lower protection`（保底、work-conserving）+ `upper bound`（封顶）覆盖了"保证你至少有多少、且不让你占超过多少"这个完整语义，而 Memtis 只有后者的一半。

**四条干扰路径的划分方法值得抄**：不是抽象地谈"干扰"，而是给出可测量的四条因果链（系统级最热 → 早到者占满 → thrashing 占带宽 → 突发分配抬高尾延迟），每条都配了量化（−28%、1/10、−7%、P99）。这种"把公平失效拆成可观测事件"的写法和 OBASE 的"热度碎片化拆成页利用率"是同一手法。

**它的推导链条也是最清楚的**：TPP 缺公平（TPP 论文自己承认"不同优先级共置时可能次优"）→ Equilibria 补公平 → 于是它唯一的基线就是 TPP。这条链说明一件事：**分层系统的研究进展是"补上一个被上一代承认的缺口"，而不是替换。**

**对我们的直接用处**：多租户公平在我们目前的 xTier/nginx 实验里完全缺席——单目标进程、单 memcached 实例。如果要往"共置"方向走，Equilibria 提供了现成的度量语言（每容器 local 份额、demotion/promotion 计数、thrashing 速率）与两个旋钮的接口设计；而它没做的事（带宽受限场景、硬件辅助）正好是空位。
