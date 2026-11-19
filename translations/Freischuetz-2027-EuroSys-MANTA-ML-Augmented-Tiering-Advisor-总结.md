# MANTA 论文总结

**MANTA: Machine Learning Augmented Tiering Advisor**
Johannes Freischuetz、Kiet Pham（UW–Madison）、Sujay Yadalam（UT Austin）、Konstantinos Kanellis（AWS）、Michael Swift、Shivaram Venkataraman（UW–Madison / ETH）
EuroSys '27（2027-04，Rabat），DOI 10.1145/3842654.3848563；arXiv:2610.00714v1

## 一句话

分层策略的损失主要来自"**反应过来太晚**"：MANTA 先造一个离线最优解 **ChOMP**（把访问图按时间分块，求解速度提高 630×，可在 30 分钟内解 10 分钟 trace），再用 LightGBM 回归**未来折扣访问次数** $V_p(t)=\sum_{i=1}^{H}\gamma^{i-1}a_p(t+i)$，把"等热度追上来"换成"提前预测"——在模拟 CXL 上 4 GB 快层比 ARMS 快 **1.12×**，在 Optane 上快 **1.69×**。

## 问题

1. 现有策略（TPP/ARMS/HybridTier/NOMAD/HeMem）都基于 recency/frequency/page-age/带宽压力的**反应式启发式**，在 phased 或 bursty 负载上热集变化快于策略适应速度。
2. **缺可靠的离线参照**：CHOPT 的访问级图对 n 次访问有 2n+2 节点、最多 4n+2 边，10 秒 trace（约 10 亿次访问）求解需 **>60 小时**；且其迁移代价是常数，忽略并发流量与带宽争用。
3. 反应式策略的两种失效：冷→热需多个控制间隔才越过提升阈值；热→冷时延迟降级继续霸占快层。更具体地，ARMS 要求 hotness score **连续两次上升**才迁移。
4. 学习型策略（Kleio/Coeus/Cronus）多停在离线或模拟器，**在线推理代价**是部署障碍。

## 机制

**① ChOMP：可解的离线最优**（p4）

把访问按固定时间步聚合——同一步内对同一页的重复访问合并为一个放置决策，按"在快层服务这些访问的总收益"加权；迁移只发生在步边界。图的规模从"访问数"降到"页 × 步"。

**② 带宽敏感的迁移代价**（p4）：用 MESS 的带宽/时延剖面估计已占带宽，再加**分段线性、边际递增**的共享干扰代价——越接近带宽上限，同一步内每多迁一页越贵。这使得模型第一次把"迁移会互相干扰"写进最优解。

**③ 提前迁移与首末放置**（p4）：允许在不被访问的步内迁移，并允许模型选初始与最终放置。经剪枝、去孤立节点、单入单出收缩后写成 **LP**（全幺模性保证整数最优），用 Gurobi 求解。相对未优化的 CHOPT 可解 **630×** 更长 trace，相对 CHOPT++ 为 **32×**；>10 分钟 trace 约 30 分钟解完。

**④ 预测目标**（p6）：$V_p(t)=\sum_{i=1}^{H}\gamma^{i-1}a_p(t+i)$，从 $t+1$ 起算、**不计当前步**，$\gamma = 0.99$（作者注明未系统调参）。

**⑤ 特征与虚拟时间步**（p6–7）：EWMA 窗口 2/5/20/100 步（继承 ARMS 的 2/20，新增 5/100）、`global_avg_accesses`、`age`、`r_ratio_20/100`、`nearby_pages`（8 个 2 MB 大页 = 16 MB）、`group_above`/`group_below`（各聚合 3 个连续邻域，每特征覆盖 48 MB）、`nearby_ewma5_var`。控制步长**不用固定 250 ms 墙钟**，而是每固定数量 PEBS 样本推进一次虚拟时间（多数 **10,000** 样本/步，FAISS 3,162，DuckDB 100,000）——让特征对执行速度不敏感。

**⑥ 模型与秩过滤**（p6–7）：LightGBM 随机森林回归，经 **lleaves** 编译为原生代码，推理降一个数量级（>10,000 页 <20 ms）；部署时对每页预测在最近 **10 个控制间隔**上平滑，再做**秩过滤**：慢层页只有落在 hot set 上半部才是提升候选，快层页只有落在 cold set 下半部才是降级候选。

## 评估设置

- **平台 1（模拟 CXL）**：CloudLab c220g5，两 NUMA 区各 Xeon Silver 4114 + 192 GB，慢层 uncore 降频至 **700 MHz**；
- **平台 2（Optane）**：Xeon Gold 6226R + 6 通道 DDR4-3200 作快层 + Optane PM 100 提供 **135 GB** 远端内存；
- **内核**：MANTA/ARMS/HybridTier 用 6.2.0-hybridtier+，另在 **Linux 6.18.1** 上重跑 ARMS vs MANTA（**沿用 6.2 的模型，不重训**）；
- **基线**：ARMS、TPP、HybridTier、NOMAD、Memtis（Optane 上只跑 ARMS，其他不支持该内核）；
- **负载**：BC-Kron/Twitter、PR-Kron/Twitter、XSBench、NAS MultiGrid、FAISS、DuckDB；足迹 13–27 GB，快层 4/6/8/10 GB，**2 MB 页**，单应用无共置；
- 每个（策略, 容量, 内核）组合 **3 次**重复；模拟器同为 trace 驱动，all-DRAM 与 all-CXL 两端点与实测差 **<0.1%**。

## 关键数字

| 指标 | 数值 | 对比 |
|---|---|---|
| 几何平均加速 @4 GB | **1.12×**（L6.2）/ **1.08×**（L6.18） | ARMS |
| 4/6/8/10 GB | 1.12×/1.07×/1.08×/1.08× | ARMS（emulated CXL） |
| Optane @4/6/8/10 GB | **1.69×**/1.13×/1.14×/1.09× | ARMS |
| 单负载最高 | 1.25×（L6.2）、**5.63×**（Optane PR-Twitter @4 GB） | ARMS |
| @4/6/8/10 GB | 1.28×/1.11×/1.03×/1.03× | Memtis |
| 对 ChOMP 的收敛 | BC-Twitter@2 GB 省 18.0 s / 缺口 21.8 s = **82%** | ARMS 与 ChOMP 之间 |
| ChOMP 可解性 | 630×（vs CHOPT）、32×（vs CHOPT++） | — |
| 在线推理 | >10,000 页 <20 ms；P95 **0.645–2.359 µs/页** | — |
| 模型构建 | 455–883 s（约 8–15 min/负载，离线一次性） | — |

## 论文自陈的局限

1. **未评估共置**：*"We evaluate applications individually, without co-running workloads."*；ARMS 限流不提供跨租户带宽分配或公平性隔离，列为未来工作。
2. **每应用一模型、依赖重训**：代码、控制流、输入类型、数据集偏斜、线程数变化都会改变访问行为；需要操作员管理生命周期（采新 trace、离机重建）。
3. **γ 未调参**：$\gamma=0.99$ 是任取值。
4. **长跑退化**：PR-Twitter 跑 100 轮（10×）时运行时间反而增加 1.7%/1.3%/2.9%/0.3%，"其 1.06–1.17× 的优势在长跑中不保持"。
5. **收益不均匀**：*"The occasional regressions, including PR-Twitter at the largest capacity, show that the benefit is not uniform."*
6. **仍差 ChOMP**：FAISS@8 GB 落后 77.3 s，因为"识对了页并不等于把它们立刻放进 DRAM"；DuckDB 反复 dealloc/realloc 丢弃有效放置，需要更好的**初始放置**。

## 我的判断

**它给了这个领域一个久缺的东西：一个"够好的最优解"作分母。** CHOPT 因为图规模爆炸而不可用（10 秒 trace 要 60 小时），ChOMP 用"时间分块 + 迁只在步边界"把规模降到可解 630×——于是"我们的策略差了 18%"这种话第一次有了明确含义。这是方法论贡献，比 1.12× 更耐久。

**最诚实的一处是长跑退化**：PR-Twitter 跑 10× 时间后优势消失（+1.7% 反而更慢）。多数论文只报短跑，而 MANTA 把这个负面结果写在正文——它说明"预测型策略在分布漂移后需要重训"不是理论担忧，是实测现象。

**对本仓其他论文的关系**：它以 TPP 与 Memtis 为基线、引用 TMO 论证成本；构建在 ARMS 之上（保留其访问追踪、迁移执行、容量管理，只替换 hotness 估计与候选选择）。它与 xTier 是最直接的对比组——**两者都在预测"未来访问"，但 xTier 把预测放进内核（34 µs），MANTA 把预测放用户态、只把排序结果用于过滤**（推理 0.6–2.4 µs/页，靠编译成原生代码）。
