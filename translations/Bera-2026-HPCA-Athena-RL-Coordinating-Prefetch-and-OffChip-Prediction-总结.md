# Athena 论文总结

**Athena: Synergizing Data Prefetching and Off-Chip Prediction via Online Reinforcement Learning**
Rahul Bera\*、Zhenrong Lang\*、Caroline Hengartner、Konstantinos Kanellopoulos、Rakesh Kumar、Mohammad Sadrosadati、Onur Mutlu（ETH Zürich / NTNU）
arXiv:2601.17615v1，2026-01-24；工件 `github.com/CMU-SAFARI/Athena`（MIT）、Zenodo DOI 10.5281/zenodo.17854634

> **元信息存疑**：PDF 首页**未印会议名/DOI**，仅 arXiv 标识；致谢提到 "reviewers of MICRO 2025 and HPCA 2026"。本仓文件名为 `-HPCA-`，但会议归属无法从 PDF 核实。

## 一句话

预取器（prefetcher）与 off-chip predictor（OCP）**互补但会互相掩埋收益**，而唯一的 OCP 感知机制 TLP 只在 L1D 层面成立——Athena 用一个 **3 KB、表格式 SARSA** 的在线 RL 同时决定"开谁"与"预取多激进"，并把"自己的功劳"与"负载自身漂移"从奖励里分离出来：100 个负载上比 Naive 组合高 **5.7%**，比 HPAC/MAB 高 **7.9%/5.0%**。

## 问题

1. **朴素并存会互相抵消**：100 个负载中 **40 个是 prefetcher-adverse**——Pythia 使这些负载平均退化 **11.6%**，而 POPET 反而提升 1.4%。Naive 组合在全部负载上仅 +4.7%，但在 adverse 负载上退化 11.2%，把 POPET 本可带来的 +1.2% 抹掉；事后按负载四选一的 StaticBest 比 Naive 高 6.5%。
2. **TLP（唯一 OCP 感知机制）只在 L1D 成立**：OCP 判定的 off-chip prefetch fill 中，L1D 有 **50.6%** 不准，L2C 只有 **28.1%**——"几乎两倍差"，所以 TLP 无法协调 L1D 以上的预取器。
3. **纯预取器协调机制（HPAC/MAB）两头不达标**：adverse 下都缓解不了 Naive 的退化，friendly 下又比不上 Naive；HPAC 根因是**静态阈值**，MAB 根因是**决策与系统级特征无关**。

## 机制

**① 建模**：agent 与处理器/存储系统交互，一个 timestep = 一个定长 epoch。选 RL 的三条理由：摆脱静态阈值、无 ground truth 故需在线反馈、Q 值天然给出动作排序。

**② 状态 4 维（由自动 DSE 从 7 个候选里选出）**：**prefetcher accuracy、OCP accuracy、bandwidth usage、prefetch-induced cache pollution**。状态维度直接决定 Q 值存储量，故刻意做小。

**③ 动作 4 选 1 + Q 值驱动的预取度**：动作 = {仅 prefetcher, 仅 OCP, 两者, 皆关}；若动作含预取，则

$$r_d = \min\left(1, \frac{Q(a^*) - \text{avg}Q}{\tau}\right),\qquad degree = r_d \cdot d_{max}$$

即"Q 值领先越多、预取越激进"。**同一套硬件同时充当协调器与节流器**，无额外开销。

**④ 复合奖励（本篇最核心的方法论贡献）**：

$$R_t = R_t^{corr} - R_t^{uncorr}$$

- **相关项**（agent 动作能影响的）：# cycles、# LLC misses、LLC miss latency；
- **不相关项**（负载自身漂移，与动作无关）：# load instructions、# mispredicted branches。

动机：既有工作只用 IPC 变化作奖励，会把 agent 的效果与负载相位变化混为一谈。

**⑤ QVStore**：不用 DNN，也不用无状态 bandit，而用**多平面哈希表**——$k=8$ 个 plane，每 plane 64 行 × 4 列，条目 8 bit，共 **2 KB**；查询时 8 个哈希并行取部分 Q 值求和，更新时每 plane 独立做 SARSA。多哈希兼顾"相似状态共享"与"相异状态去别名"。

**⑥ 在线测量**：预取准确率用 Bloom filter（记已发预取地址，demand 命中算准，每 epoch 重置）；OCP 准确率用两个计数器；**预取污染**用另一个 Bloom filter（LLC 为预取而逐出的块地址入 filter，后续 miss 命中即计数）。各 4096 bit、0.5 KB。

## 评估设置

ChampSim（Golden Cove 类：6-wide、512 项 ROB、L1 48 KB、L2 1.25 MB、LLC 3 MB/core、DDR4 3.2 GB/s per core）。**无真实硬件、无硅验证**。基线：`Naive`、`StaticBest`（事后四选一，作上界参照）、`TLP`、`HPAC`（本文扩展到协调 OCP）、`MAB`（DUCB）。预取器 6 个（L1D：IPCP、Berti；L2C：Pythia、SPP+PPF、MLOP、SMS），OCP 3 个（POPET、HMP、TTP），缓存设计 4 种（CD1–CD4）。负载 **100 条**内存密集 trace（SPEC 2006/2017、PARSEC、Ligra、CVP，≥3 MPKI 门槛）+ **359 条** Google DPC4 仓库级 trace（完全不参与调参）。调参只在 **20 个不在测试集内**的负载上做，得到的配置原封不动用于其他配置。

## 关键数字

| 指标 | 数值 | 对比 |
|---|---|---|
| 100 负载 geomean（CD1） | **+5.7% / +7.9% / +5.0%** | Naive / HPAC / MAB |
| prefetcher-adverse 负载 | Naive **−11.1%** vs Athena **+14.0%** | 无预取无 OCP 基线 |
| 与 StaticBest 的距离 | 1.103 vs 1.111（+10.3% vs +11.1%） | StaticBest |
| CD4（L1D+L2C 各一） | Naive **−26.8%**、TLP −16.7%，Athena 比 TLP **+19.9%** | TLP / Naive |
| 1.6 GB/s 带宽 | Naive **−18.9%**、POPET −1.1%，Athena 比 Naive +21.4% | — |
| 12.8 GB/s 带宽 | Naive +33.5%，Athena 比 MAB +11.6% | Naive / MAB |
| 硬件开销 | **3 KB/核**（QVStore 2 KB + 2×0.5 KB） | Pythia 25.5 KB、TTP 1536 KB、TLP 6.98 KB |
| 四核/八核 | 比 Naive +5.3% / +9.7%，比 MAB +3.0% / +4.3% | Naive / MAB |
| 主存请求数 / LLC miss 延迟 | Naive +21.9% / +28.3%，Athena +5.8% / **+1.7%** | 无预取基线 |
| 359 条未见 trace（CD4） | Athena **+2.8%**、TLP +2.2%、MAB +0.1%、HPAC −1.3%、Naive −2.1% | 各基线 |

## 论文自陈的局限

1. **全部为模拟结果**：无硅验证、无功耗/面积（只有存储开销）。
2. **多核未专门调参**：超参来自单核 DSE，直接套用；作者相信多核调参还能更好。
3. **无 OCP 时只能止损**：*"Athena only prevents performance loss as opposed to improving performance."*
4. **调参规模有限**：20 个负载、单核、仅 CD1、仅 POPET+Pythia 组合，其余配置一律"applied unaltered"——泛化靠实验证明而非调参保证。
5. **无状态版本不如 MAB**：ε-greedy 的探索策略本身是短板（MAB 用 DUCB）。
6. **带宽极度受限时两者都无益**：POPET 在 1.6 GB/s 下 −1.1%。
7. **QVStore 更新延迟是建模值**（50 周期），非实测。

## 我的判断

**它给出了"奖励该怎么设计"的一个可复用模板。** 只用性能作奖励的 RL 系统（包括本仓的 xTier、IDT、ReStore）都面临同一个问题：负载本身在漂移，你怎么知道性能变化是你的功劳？Athena 的做法是把指标**分成两组**——动作能影响的（cycles、LLC miss、miss latency）与动作不可能影响的（load 指令数、分支预测错误）——然后用后者做基线扣减。这个"减去不相关项"的思路，比任何调参都更根本。

**它也解释了为什么"组合两个好机制"经常失败**：不是两者不互补，而是**没有一个组件知道另一个的存在**。这与 Harmonia 的结论（放置与迁移必须拆成两个 agent 并用奖励耦合）互为印证，只是层次不同：Harmonia 在系统层、Athena 在微架构层。

**与本仓其他论文的关系**：参考文献 208 条中**只引用了一篇**本仓论文——**Harmonia**（ref [135]），被归入 "ML for hybrid memory/storage management" 并明确声明"与 Athena 正交"。它与 TPP/TMO/Memtis/OBASE/xTier/Camp/Equilibria 均无引用关系。值得注意的是作者重叠（Bera、Sadrosadati、Mutlu），这条引用更像是同组工作的互相标注，而非技术依赖。
