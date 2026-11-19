# ReStore 论文总结

**ReStore: A Reinforcement Learning Approach for Data Migration in Multi-Tiered Storage**
Tianru Zhang（Uppsala）、Tarikul Islam Papon（UMass Boston）、Teona Bagashvili、Manos Athanassoulis（BU）、Salman Toor（Uppsala）
Proc. ACM Manag. Data 4(3), SIGMOD 2026, Article 227，DOI 10.1145/3802104；开源：`github.com/BU-DiSC/ReStore`

## 一句话

**每层一个 RL 智能体**、共享一个"最小化响应时间"的奖励，用 **TD(λ) + 模糊规则基值函数（每层只有 4 个可训练参数）** 做页级迁移决策——在 TPC-C/TPC-E/YCSB、Google Thesios 与 MSR Cambridge trace 上，**运行时最多降 6×、迁移次数最多降 48×**。

## 问题

1. **规则式策略（LRU/LFU/EXD/LRFU）** 难以适应 workload drift，也忽略 SSD 特性。
2. **设备属性未被刻画**：Intel P4510 在满并发下读带宽比无并发高 **40×**，4 KB 写比 4 KB 读慢 **3×**——这些在多层存储里全是可利用的自由度。
3. **迁移粒度过粗**：现有方案多为文件/对象级（Ceph、S3 lifecycle）；企业方案到块级但依赖静态启发式或固定观察窗；DBMS/KV-store 以 **4 KB 页**为操作单位，需要页级、运行时区分冷热的策略。
4. **页级决策必须轻量**：页数远多于文件，复杂 ML 难以逐页套用；策略必须同时"够复杂以刻画漂移与设备属性"和"够轻量以高频决策"。

## 机制

**① 协作式多智能体 MDP**（p9）：每层一个 agent，**所有 agent 共享同一 reward**（最小化整体响应时间）；动作为 $\{K, A_t\}$，$A_t \in \{0,1\}$（0=不迁，1=升级到更快的第 $i+1$ 层）；层数变化只是增删 agent。

**② 两个状态变量**（p10）：

- $s_1 = \frac{1}{|K|}\sum_K temp(K)$：层内页的平均温度（融合 recency + frequency）；
- $s_2 = \text{num\_of\_queued\_tasks} \times avg\_process\_time$，其中 $avg\_process\_time = k_r l_r + \alpha k_w l_w$——$k_r/k_w$ 是读/写并发度，$\alpha$ 是读写不对称度，$l_r$ 是读延迟。**设备属性被直接写进状态**，这是它与多数内存分层工作的最大区别。

**③ 温度模型**（p14–15）：升温 $temp = 1-\delta e^{-0.5\,req}$（$\delta$ 典型 0.05–0.2）；降温：每累计"总页数的 1/10"次访问未被触及则降 $\Delta = 0.02$，下限 0.01。

**④ 模糊规则基值函数**（p11–12）：输入 $(s_1,s_2)$，每维分 Small/Large，共 $2^2=4$ 条规则；隶属函数 $\mu_L(x)=1/(1+a e^{b x})$，参数自适应 $b=10/\text{range}(x)$、$a=e^{b\cdot avg(x)}$；值函数 $v(s)=\sum_i p^i w^i(s)/\sum_i w^i(s)$——**可训练参数只有 4 个**。这就是"够轻量"的来源。

**⑤ 升级判据**（p12–13）：当

$$q^i(s,1) + q^{i+1}(s,1) \ge q^i(s,0) + q^{i+1}(s,0)$$

即"两层的升级价值之和 ≥ 不升级价值之和"时迁移；快层满则把最冷页降级。

**⑥ TD(λ) 在线更新**（p13–14）：$\gamma = e^{-\beta\tau_n}$（**连续时间折扣**，适应不规则到达）、资格迹 $z_n(s)=\lambda e^{-\beta\tau_n}z_{n-1}(s)+\mathbb{1}(s=s_n)$，学习率 $\alpha_n = 0.1/(1+100\sum\phi^i(s_t))$；基函数线性独立时收敛。

## 评估设置

三套平台：① 自研 C++ 三阶存储模拟器；② **FEMU**（FAST'18 flash 模拟器）四组配置（读写延迟 30/200/500 µs、不对称 1.5/2/4、并发 8/4/2 等）；③ 云上 **OpenStack QoS 卷**（快 33,333/22,222 IOPS、中 5,000/2,500、慢 2,000/500）。容量：Tier1 = 总量 1/3/6/12%，Tier2 = 4/8/16%，Tier3 = 100%。

基线 **8 个 + Oracle**：tLFU、tLRU、EXD、LRFU、XGBoost、Logistic Regression、TEMP（仅温度模型消融）、**Sibyl**（块级 RL 分层，用其公开实现）。

负载：合成静态（`5hf90`/`10hf90`/`10hf80`）+ 合成动态（A–D）+ 真实 trace（TPC-C、TPC-E、YCSB、**Google Thesios**、MSR Cambridge 两条），统一按 4 KB 页转换；最大 trace 有 6200 万页、9270 万次操作。

## 关键数字

| 指标 | 数值 | 对比 |
|---|---|---|
| 运行时（上限） | **6×**（正文细化为 6.1×） | 全部基线 |
| 迁移次数（上限） | **48×** | tLRU |
| `10hf90` 运行时 | −5% / −40% / −23% / −7% / −49% / −31% / −13% | tLFU / tLRU / LRFU / EXD / Logi / XGB / TEMP |
| 迁移次数 | 8× / 48× / 19× / 7× / 53× / 27× / 21× | 同上 |
| 动态负载（A/B/C/D） | 2.81× / 1.03× / 3.51× / 4.28× | LRFU |
| 动态负载总体 | 6.1× / 5.3× / 4.9× / 6.0× / 2.1× / 1.6× / 1.4× | tLFU/tLRU/LRFU/EXD/Logi/XGB/TEMP |
| 规模扩展（1e4→1e7 页） | 归一化延迟 1.03→1.12 | Oracle；同期 LFU 1.1→1.45、LRU 1.35→1.55 |
| 单请求开销 | **4.358 µs / 2.349 MB** | Oracle 0.701 µs；TEMP 1.860；XGB 4.013 |
| 单请求平均耗时 | **0.801 µs** | Sibyl 27.356 µs（约 34×） |

## 论文自陈的局限

无独立章节，逐条摘录：

1. **CPU 开销偏高**：*"While ReStore achieves good runtime improvements, it comes with an increased CPU cost."*；多个 RL agent 使其比 TEMP 更重。
2. **温度模型需按负载标定**（$\delta$、冷却频率、$\Delta$），虽然作者称对取值稳健。
3. **ML 基线只在前 5% trace 上训练**（因训练时间与内存可行性的限制）——对比设置本身受影响。
4. **与 Sibyl 的对比不同构**：ReStore 页级、Sibyl 成组迁移；且为匹配 Sibyl 需**强制所有策略单线程**。
5. **设备鲁棒性一节缺图**：*"We exclude the figure for brevity."*
6. **云上实验只跑三组负载**（`10hf90`、MSR_hm_1、Google Thesios）。
7. **Markov 假设**：假设连续访问之间页请求相互独立。
8. **聚焦 SSD 层**（*"we primarily focus on better exploiting SSDs"*），HMM 类层级仅作补充实验。

## 我的判断

**它把"设备属性"当成一等状态变量，这一点在本仓全部论文里是唯一的。** 其他工作（TPP/Memtis/OBASE/xTier）的状态是"页有多热"；ReStore 的 $s_2$ 明确含读写不对称、并发度、排队任务数——**因为它的慢层是 SSD，写比读贵 3×**。这正是内存分层论文不需要考虑、而 flash-as-memory（MEXT 路线）必须考虑的东西。

**"4 个可训练参数"是它最漂亮的工程决定**：模糊规则基函数把值函数压到 4 维，于是 TD(λ) 可以在线更新而不需要神经网络。代价是表达能力受限（只有 Small/Large 两档），收益是单请求 0.8 µs、比 Sibyl 快 34×。这与我此前总结的 IDT 是同一个思路的两种极端：IDT 压**动作空间**（三个分位里选一个），ReStore 压**值函数参数空间**。

**与本仓八篇的关系**：全文**未引用** TPP/TMO/Memtis/OBASE/xTier/Camp/Equilibria/Harmonia，且**全文未出现 CXL**——它的"多层存储"指 SSD 层级而非内存层级。唯一被作为基线的 RL 分层工作是 **Sibyl**（ISCA'22），而 Sibyl 恰好是 Harmonia 的起点。若要串起一条线：**Sibyl（块级 RL 放置）→ ReStore（每层一 agent）→ Harmonia（每任务一 agent + 奖励耦合）**，"按层/按任务拆 agent"是这条线的主轴。
