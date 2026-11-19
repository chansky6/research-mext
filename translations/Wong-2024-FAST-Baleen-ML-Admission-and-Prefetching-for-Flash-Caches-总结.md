# Baleen 论文总结

**Baleen: ML Admission & Prefetching for Flash Caches**
Daniel Lin-Kit Wong（CMU）、Hao Wu、Sathya Gunasekar、Jimmy Lu、Snehal Khandkar、Abhinav Sharma（Meta）、Carson Molder（UT Austin）、Daniel S. Berger（Microsoft & UW）、Nathan Beckmann、Gregory R. Ganger（CMU）
FAST '24，pp. 347–363；开源：`github.com/wonglkd/Baleen-FAST24`

## 一句话

Flash 缓存的瓶颈不是命中率而是**后端磁盘的峰值寻道时间**与**闪存写寿命**：Baleen 把"接纳"建模成 **episode**（一次接纳后本该命中的整串访问）级背包问题，用 LightGBM 学"OPT 会不会接纳"，并只在收益超阈值时做范围预取——固定写速率下峰值 DT 降 **12%**，TCO 降 **17%**。

## 问题

1. **HDD 吞吐跟不上容量**：单盘约 100 IOPS，随机负载下寻道惩罚与大小无关（约 10 ms），单 IO 可达 10–70 ms；瓶颈指标是**峰值** disk-head time 而非平均延迟。
2. **闪存写寿命限制写入率**：全量接纳需 492 MB/s ≈ **43 DWPD**；按 3 DWPD/5 年额定，寿命降到约 **4 个月**（快 14×）。
3. **ML 接纳策略的四个陷阱**：优化指标选错（IO 命中率 ≠ DT）；误判代价不对称（FP 付全量写成本）；**类别极不平衡**（94% 样本为负类，过采样/欠采样/加权均无效）；AP 只作用于 miss，训练集只能靠在线模拟生成。
4. **预取代价高**：错误预取同时消耗写预算与额外 DT，且机会成本是与"接纳 miss 块"竞争同一份额度。

## 机制

**① 把 DT 变成可算的指标**（p350）

$$DT_i = t_{seek} + n\cdot t_{read},\qquad Util_{DT} = \frac{\sum DT_i}{DT_{Provisioned}}$$

$\sum DT_i = Fetches_{IOs}\cdot t_{seek} + Fetches_{Bytes}\cdot t_{read}$；取每 10 分钟窗口的 P100 作 $PeakDT$。仅用两个生产计数器即可算，与 Meta 生产集群上报的磁盘利用率**峰值误差 1% 内**。

**② TCO 模型**（p350）相对式 $TCO \propto PeakDT\cdot R_1 + FlashWR\cdot R_2$，参数取"每节点 1×1 TB SSD + 36×10 TB HDD"；关键结论：**flash 写入只占 TCO 的 2–5%**——所以"用写额度换 DT"这笔交易基本总是划算的。

**③ Episode 驻留模型（核心贡献，p351）**：episode = "若该 item 被接纳，则其后本该命中的那串访问"；**size** = 覆盖这些访问所需的最少 segment 数，**timespan** = 首访问到末次淘汰。恒定 eviction age 假设下各 episode 决策**相互独立、可并行**；对假定 EA（典型 ≥2 h）不敏感，只要不是秒–分钟级。

**④ OPT：episode 级近似最优**（p352）：按 $Score = DT_{Saved}(E)/Size(E)$ 排序，在写预算内尽量多取。

**⑤ ML 接纳**（p352–353）：9 个特征（namespace 基数、user 基数、temporary 标记 + 最近 1–6 小时的 IO 数与 segment 访问数，count-min-sketch 六格队列）；**二分类**（回归端到端更差）、标签 = "OPT 是否接纳"、**每个 episode 只取前 6 次访问**以抵消对热门 episode 的偏置；双层收敛循环（EA 初值 2 h → 生成 episode → 训练 → 模拟测平均 EA → 回代）。LightGBM，500 轮、63 叶子；推理约 **30 µs/次**、每 miss 4 次推理、CPU 额外 **1%**。

**⑥ 预取 ML-Range + ML-When**（p353）：两个回归模型预测 episode 的起止 segment；仅当预测收益 > **5 ms**（对照：一次 IO 寻道 12 ms）才预取；underfetch 记 0、overfetch 按 DT 损失计。

**⑦ Baleen-TCO**（p354）：在写速率扫描范围内模拟，取使 TCO 最小的写速率。

## 评估设置

生产参数：Tectonic 节点 378 TB HDD / 400 GB flash / 10 GB DRAM（37,800:40:1）、8 MB block、128 kB segment。测试床：24 节点（16 核 E5-2698、64 GB、P3600 400 GB NVMe、4 TB HDD），实验只用单节点。基线：**CoinFlip、RejectX、Flashield、CacheLib-ML**，另有 oracle OPT / OPT-Range。负载：**7 条 Meta Tectonic 生产 trace**，跨 2019/2021/2023 三个年份，请求率 106–426 s⁻¹，one-hit-wonder 占 48–83%。仿真用 0.1–5% 采样，第 1 天训练、之后测试。

## 关键数字

| 指标 | 数值 | 对比 |
|---|---|---|
| Peak DT（固定写速率） | **−12%**（各 trace 5–29%） | RejectX |
| 峰值负载 | **−16%** | 最佳基线 |
| TCO | **−17% / −18%** | CacheLib-ML / RejectX |
| 等效容量 | 容量 **−55%** 保持同 Peak DT（≈4× 容量） | RejectX |
| 预取收益 | Peak DT **−16%**（比 All-on-Partial-Hit 再 −4%） | 无预取 |
| 与 OPT 剩余差距 | **16%**：晚接纳 9%、segment 粒度 11%、早淘汰 11% | OPT |
| 写寿命压力 | 全量接纳 43 DWPD → 3 DWPD/5 年只剩 4 个月 | SSD 额定 |
| Flashield | 半数 trace 样本无法训练；可训练负载上 Baleen 领先 18% | Flashield |
| 推理/CPU | 30 µs/次、每 miss 4 次、CPU +1%、元数据 <1 kB | — |

## 论文自陈的局限

1. **没有为峰值显式优化接纳**：*"As explicitly optimizing admission for the peak introduces significant complexity, we leave that for future work"*——只按时段限流"did not reduce Peak DT"。
2. **淘汰策略未用 ML**：仍用 LRU，flash-aware 淘汰留作未来工作。
3. **eviction age 假设为常量**，现实中随时间变化。
4. **与 OPT 差 16%**，且模型学成"几乎拒绝所有首次访问"的贝叶斯最优行为，作者判断需要更好的元数据特征才能分辨少数真阳性。
5. **block 级粒度**：segment 级潜在 11% 收益"未能实现"；早淘汰与 Prefetch-on-PUT 各 11% 也因模型精度不足未实现。
6. **以仿真为主**，测试床只验证个别点；Baleen 本体尚未在生产部署。

## 我的判断

**它最硬的贡献是"把指标换对"**：从 IO 命中率换成 Peak DT。理由很物理——HDD 的寻道成本与请求大小无关，所以"命中数"与"总吞吐成本"之间的换算根本不成立；而峰值（P100）而非均值才是需要扩容的那一点。这一点比任何 ML 细节都重要。

**"episode"是把在线决策变成可离线监督的关键抽象**：只有假定 eviction age 恒定，episode 之间才独立，才能预先算出"接纳它的收益 ÷ 它的代价"；训练标签（OPT 是否接纳）也才有定义。代价是 16% 的剩余差距——作者诚实列出了三段损失（晚接纳 9%、segment 粒度 11%、早淘汰 11%，有重叠）。

**与本仓其他论文的关系**：全文**未引用** TPP/TMO/Memtis/OBASE/xTier/Camp/Equilibria/Harmonia 中的任何一篇——它是 flash 缓存接纳方向，与内存分层无交集。但它与本仓 Harmonia 构成对称：**Harmonia 是"放置 + 迁移"两智能体，Baleen 是"接纳 + 预取"的两段式**，且都强调"误判代价不对称"。若要讨论"分层里的决策该不该拆"，这两篇是一组很好的对照。
