# IDT 论文总结

**IDT: Intelligent Data Placement for Multi-tiered Main Memory with Reinforcement Learning**
Juneseo Chang、Wanju Doh（首尔大学）、Yaebin Moon（三星电子）、Eojin Lee（仁荷大学）、Jung Ho Ahn（首尔大学）
HPDC '24，2024-06，pp. 14，DOI 10.1145/3625549.3658659；开源：`github.com/scale-snu/IDT`

## 一句话

在**真实的四层内存**上做 RL 分层：用 PPO 学"每个层各自该用多激进的降级阈值"（动作就是从前一窗口的 age 分布分位里挑一个），配区域粒度监控与 Fisher 检验式区域合并——比默认 Linux 快 **2.08×**，比当时最好的多层方案（MGLRU）再快 **11.2%**。

## 问题

1. **多层比两层更难**：多数 OS 级方案只支持两层；已有的多层方案在降级 fallback 路径中常**绕过最快层**，性能反而不如两层方案。
2. **热度判定不准**：Linux 的 2Q LRU / MGLRU 判出的热度与 PTE access bit 的真实热度偏离；且各负载"被访问页 age 的累积分布"陡峭度差异大，`age_thres` 必须在线自适应，**而且每个层要有各自的阈值**。
3. **已有 ML 方案开销不可接受**：Kleio 逐页在线 LSTM 要训 2 小时、占数十 GB；Coeus 仍需训练/推理大量模型；Google WSC 的 GP-Bandit 搜参"要一天"。作者据此明确**不与之直接对比**（"not suitable"）。
4. **区域粒度监控的 merge/split 缺依据**：DAMON 在频率差 < 全局最大 freq 的 10% 时合并、随机拆分；MTM 用 scan count 的 1/3、2/3 阈值——论文称这些阈值"lack a logical basis"。

## 机制

**① RL 形式化（p4–5）**：目标是 $\arg\max_\pi \frac{1}{T}\sum_t \left[(1-\beta)\cdot Perf(t) + \beta/Mem(t)\right]$，用 PPO 限制策略更新幅度。为简化，把 `demote_wmark` 设得比既有工作更高，在快层未满时就主动降级，从而消去 $Mem(t)$ 项——**把多目标压成单目标：最大化性能**，`Perf(t)` 用慢层访问频率近似反比。

**② 区域粒度监控（p5）**：VMA 切成 region；`sample_interval = 10 ms`、`aggregate_interval = 1000 ms`；每 region 取两页采样页，用 PTE access bit 判访问。维护 `freq`、`age`（沿用 DAMON 算法）、`history`（**1024 位**向量，记录每个采样周期是否被访问）；另记每层的 `demoted_pages` 与 `slow_hit`。

**③ RL 驱动的降级（p5–6）**

- **State**：age 分布的 min/q1/q2/q3/max 的滑动平均（PCA 剔除离群），5 个输入；
- **网络**：16–32 两层全连接，输出 3 个节点（对应 q2/q3/max），**取最大者作为新的 `age_thres`**——动作空间就是"三个分位里选一个"，这也是它能做到低开销的关键；
- **Reward**：$reward = \log\frac{demoted\_pages+\epsilon}{slow\_hit+\epsilon}$，即"降级的页里有多少真的没被再用"；
- **超参**：$\gamma=0.9$、lr $0.01$、$\epsilon=0.05$、buffer = batch = 4、`action_interval = 2 s`；
- **开销**：单核推理 **2–4 ms**、每 4 次推理训练一次（200–300 ms），理论上限 3.95% 单核算力；
- **多层**：每层独立推理、**共享一个网络**（训练时汇总各层经验）；用 GUPS 微基准（Uniform / Hot set / Dynamic hot set，100 GB 工作集）预训练。

**④ 降级执行（p7）**：`capacity(nid) < demote_wmark` 触发；候选 = `freq == min_freq` 且 `age ≥ age_thres`，用 `migrate_pages()` **自最低层向下一层**逐层迁移。`capacity < critical_wmark` 时进入激进模式（`freq_thres` 取 min/max 中值），仍不足则尝试降级全部 region，最后由 kswapd 接管。默认 `demote_wmark = 10%`、`critical_wmark = 1%`。

**⑤ 提升（p7–8）**：ARP（被访问的 region 整体提升到最高层；若空间不足则升到次高层防抖）+ **PRP（Prefetch-like Region Promotion）**：用 k-NN 在同一源层里找最相似的 region 一并提升，距离函数把虚拟地址间隙与 history 的 Hamming 距离各自标准化后相加。另有 "misplaced region promotion"，处理被 kswapd 抢先降级的 region（平均占总提升 **9.71%**）。

**⑥ 区域重构（p8）**：合并用 **Fisher 精确检验**——在 history 上以窗口 $n=16$、步长 1 滑动，$p = \binom{n}{a_1}\binom{n}{a_2}/\binom{2n}{a_1+a_2}$，**所有窗口**都在 90% 显著性水平拒绝原假设才合并；拆分则是两采样页在某采样周期访问状态不同时对半切。

## 评估设置

- **内核**：Linux v6.0.19（监控基于 DAMON 实现），对照的 MGLRU 用 v6.1.0；
- **硬件**：2 × Xeon Platinum 8260（24 核/颗），每 socket 32 GB DDR4 + 256 GB Optane DCPMM（App Direct，暴露为 NUMA 节点）；
- **真实四层延迟**：DRAM0 **90 ns** / DRAM1 145 ns / DCPMM0 275 ns / DCPMM1 **340 ns**；
- **基线**：Intel Tiering 0.8、TPP（均为两层）、AutoTiering OPMX、AutoNUMA Tiering (MGLRU)（多层），归一化到 vanilla 默认 AutoNUMA Balancing；
- **负载**：Graph500；SPECspeed 2017 的 deepsjeng、imagick、bwaves、xz、roms、cactuBSSN；GAPBS 的 bfs、pr。RSS 统一 96–110 GB（实际用到三个层）；
- **控制变量**：`numactl -c0` 绑核、关 HT/DVFS/Turbo/预取；指标 = 执行时间倒数。

## 关键数字

| 指标 | 数值 | 对比 |
|---|---|---|
| 平均加速比 | **2.08×** | 默认 Linux（AutoNUMA Balancing） |
| 相对最佳 SOTA | **+11.2%** | AutoNUMA Tiering (MGLRU) 的 1.87× |
| 其他基线 | 1.48× / 1.48× / 1.56× | Intel Tiering / TPP / AutoTiering |
| graph500 | +23.5% | Intel Tiering |
| RL 单核 CPU | 平均 **1.35%**，峰值 3.75% | 单核 |
| RL 峰值内存 | 4,776 MB（总内存 **0.83%**） | 因置于 tier3 |
| PRP 准确率 | 平均 **87.0%**（71.4–100%） | 无 PRP 时整体为 0.97× |
| 区域合并后的 history Hamming | **5.15** vs DAMON 8.13 | IDT-DAMON |
| 两层配置 | All-uppermost-tier 的 **81.3%** | 与 MGLRU 相当 |

## 论文自陈的局限

1. **黑箱**：*"IDT's limitation is its black-box nature... the inherent opacity of deep RL models makes it difficult to explain the exact reasons for such improvement."*
2. **用户态运行动作，内核态才有机制**：策略在用户态、且绝大多数内核策略都在内核态；作者提到可借助新出的内核 ML 框架移入内核。
3. **奖励里的 `slow_hit` 是近似值**：精确的慢层访问信息在默认内核里不易获得，用 perf 采会引入额外开销、低开销传回 RL 模型"technically challenging"。
4. **曾试过的替代奖励更难**：以 demote→promote 间隔（DtoP）为奖励需要等被降级的 region 再次被提升，奖励延迟过长、更易受非平稳环境伤害。

## 我的判断

**它把"动作空间"压到了工程可承受的极限**：不直接输出阈值，而是从 age 分布的三个分位里挑一个。这一招同时解决了两件事——推理只需 2–4 ms、动作永远落在**合理区间内**（不会学出荒唐阈值）。这比"网络小一点"是更本质的开销控制。

**四层是它真正的论据**：市面上多数工作停在两层（TPP/Intel Tiering），而多层方案在 fallback 路径上常绕开最快层、反而不如两层。IDT 用**真实四层硬件**（90/145/275/340 ns）证明"层数增加时自适应阈值的价值递增"——这是纯模拟工作做不到的。

**代价是解释性**：作者自己承认模型不可解释。对我们的实验设计有直接参考价值：如果要复现，**必须同时记录慢层命中率（slow_hit）这个内部量**，否则奖励的正确性无法验证——而这恰恰是它承认只能近似的地方。

**与 TPP 的关系**：TPP 是它的基线之一（1.48×），但 IDT 明确批评 NUMA hinting fault 在关键路径上有开销——这与 xTier"用户态控制器错过 10 ms 窗口"是同一类批评，两条路线（更聪明的用户态策略 / 搬进内核）在这里分叉。
