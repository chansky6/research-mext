# ExPAND 论文总结

**CXL Topology-Aware and Expander-Driven Prefetching: Unlocking SSD Performance**（系统名 **ExPAND**）
Dongsuk Oh、Miryeong Kwon、Jiseon Kim、Eunjee Na、Junseok Moon、Hyunkyu Choi、Seonghyeon Jang、Hanjin Choi、Hongjoo Jung、Sangwon Lee、Myoungsoo Jung（Panmnesia / KAIST）
IEEE Micro 收录论文的 arXiv 扩展版，arXiv:2505.18577v1，2025-05-24

> 元信息说明：PDF 首页仅印 arXiv 版本与"本稿为 IEEE Micro 录用论文的扩展版"，**未见期刊卷期/页码/DOI**。

## 一句话

CXL-SSD 池化把容量做大了，但 CPU 侧的预取器既看不到**拓扑层数**、也没有预算做 ML——于是 ExPAND 把预取**下放到 CXL-SSD 控制器**（transformer + 行为检测），用刷新缓冲把数据"提前送到 host 侧"，并用 PCIe/CXL 配置空间把**端到端延迟**告诉决策器：图应用 **9.0×**、SPEC CPU **14.7×**。

## 问题

1. **CXL-SSD 的后端介质远慢于 DRAM**：PRAM 可达 DRAM 的 7×，Z-NAND 与 XL-Flash 慢约 **30×**；SSD 内部 DRAM 缓存能降写延迟，但对慢速 SCM 造成的读延迟不够。
2. **低局部性下性能不可接受**：$\alpha=0.1\sim1$、$L=4\sim16$ 时平均比 LocalDRAM 慢 **738%**；局部性高时差距缩到 35%。既有 CXL-SSD 研究只优化内部 DRAM 缓存利用率，**忽视缓存命中率**。
3. **CPU 侧预取器不够用**：规则式预取器准确率仅 9–76%，且需数十 MB 存储；ML 预取器准确率够，但片上面积/元数据开销不可行。
4. **多层 CXL switch 造成延迟差异**：CXL 3.0/3.1 支持多级交换（每 VH 最多 4K 设备），每层引入处理延迟；不感知拓扑的预取，会在 CPU 需要时数据尚未到达——**命中被转成缺失**。

## 机制

**① 分离式 reflector / decider**（p4–5）：reflector 位于 host 侧 CXL root complex 与 LLC 控制器，用 **16 KB** 小缓冲记录 decider 预取来的 cache line；LLC 先查该缓冲，命中就直接从 RC 供数、**不穿越 CXL-SSD 池**。decider 位于 CXL-SSD 控制器，承载异构 ML 预取器。

**② 地址预测器**（p5）：transformer 作序列模型，引入 **multimodality attention** 建模访存模式与 PC 的关系；attention dim 64、fusion dim 128、transformer dim 128。

**③ 行为变化检测**（p5）：决策树分类器**预训练为 64 类**，用滑窗内的地址 + PC 在线推断；类别跳变即 behavior-change event，作为 hint 送入 transformer。

**④ 时序预测器**（p5）：×80 B 缓冲、10 条 timing 记录；LLC 直接命中时请求不达 decider，故 reflector 经 **CXL.io** 把 cache-hit 事件告知 decider。

**⑤ 拓扑感知的"及时性"判据**（p5–6）：PCIe 枚举时按 bus number 切分/编号总线（CXL switch 呈 PCIe bridge），由此确定 host 与目标 CXL-SSD 之间的**交换层数**；再从设备 DOE 取 **DSLBIS** 得设备级延迟与带宽，拼上 VH 内 switch 开销，把**端到端延迟写进该设备的 PCIe 配置空间**。判据 = 时序预测值 − 端到端延迟：过早污染 LLC，过晚数据不可用。

**⑥ 双向 CXL 通信原语**（p6）：下行在 M2S 的 **RwD** 里新增 **MemRdPC** opcode，让 LLC 缺失请求随包携带当前 PC；上行复用 S2M 的 **BISnp** 新增 **BISnpData**（最多 10 个 custom opcode）把预取数据插入 reflector 缓冲。一致性依赖 CXL 3.0 的 back-invalidation。

## 评估设置

无商用 CXL-SSD，故用 **gem5 + SimpleSSD** 全系统仿真（CXL 硬件 RTL 已在真实 CXL 系统上做过 cycle 级校验）。主机：12 核 @3.6 GHz、L1D 48 KB、L2 1.25 MB、DRAM 22 ns、**PCIe 6.0 + CXL 3.0**。CXL-SSD：Samsung 983 ZET 2 TB，tRd 3 µs / tWr 100 µs，内部 DRAM 1.5 GB。基线：`NoPrefetch`、`Rule1`（Michaud best-offset）、`Rule2`（Jain & Lin）、`ML1`（LSTM）、`ML2`（transformer）、`LocalDRAM`。负载：4 个图算法（CC、PR、SSSP、TC）× 5 个数据集 + SPEC CPU（bwaves、leslie3d、lbm、libquantum、mcf）。后端介质对照：ExPAND-Z（Z-NAND）/ -P（PMEM）/ -D（DRAM）。

## 关键数字

| 指标 | 数值 | 对比 |
|---|---|---|
| 图应用 | **9.0×** | NoPrefetch |
| SPEC CPU | **14.7×** | 其他预取策略 |
| 相对已有 ML 预取器 | **2.4×** | ML1 / ML2 |
| 图负载 vs LocalDRAM | 仍慢 **48%**（LLC 命中 86%、miss 14%） | LocalDRAM |
| SPEC vs LocalDRAM | 平均提升 46%；leslie3d 3.9×、lbm 2.8×、libquantum 1.2× | LocalDRAM |
| 预取有效性阈值 | 及时率 <80% 时最多慢 **4.5×**；>90% 才显著收益 | LocalDRAM 归一 |
| 每多一层 switch | 平均退化 **2.7×**（CC/PR/TC 1.3×/层，SSSP 1.4×/层） | 0 层 |
| 局部性极值 | 低局部性慢 **738%**；高局部性慢 35% | LocalDRAM |
| ExPAND-D | 1.3–3.9×，平均 1.9× | LocalDRAM |
| 预取器开销/精度 | 存储 **839.2 KB**、10.3M IOPs、准确率 **92%** | ML1 936.8 KB/88%、ML2 865 KB/89% |

## 论文自陈的局限

1. **无真实 CXL-SSD 硬件、全仿真**：*"Since no CXL-SSDs are currently available, we use CXL hardware RTL modules in a full system simulation model."*
2. **完美预取不现实**：*"While unrealistic, a perfect prefetch allows the CXL-SSD to outperform LocalDRAM."*
3. **图负载仍不及本地 DRAM**：比 LocalDRAM 慢 48%，因为仍有 14% 的 miss。
4. **低局部性场景不可接受**：比 LocalDRAM 慢 738%。
5. **时序精度饱和**：收益在约 68% 及时率后饱和，84% 以上边际很小；且模型假设"LLC 访问频率在负载执行期间相对恒定"。

## 我的判断

**它解决的是别人不认为是问题的问题：预取的"准时"而非"准确"。** 在多级 CXL 交换下，同一个地址的预取提前 50 µs 与提前 500 µs，后果完全不同——前者污染 LLC、后者数据还没到。把**端到端延迟写进设备配置空间**、让决策器读它，这个做法把"拓扑"从一个隐含假设变成了可查询的量，是本篇最可借鉴的工程手法。

**"把 ML 从 CPU 挪到设备"这一选择值得单独记住**：CPU 侧 ML 预取器的存储需求（数百 KB 到 MB）在片上不可行，而 CXL-SSD 控制器有算力与空间。这与 PF-LLM 的"把 LLM 挪到离线"是同一策略的两个变体：**凡是不属于关键路径的复杂度，都该搬走**。

**与本仓其他论文的关系**：参考文献 46 条中**未引用** TPP/TMO/Memtis/OBASE/xTier/Camp/Equilibria/Harmonia，属 CXL 存储侧硬件预取方向；它与上述八篇的唯一交集是"慢层介质"这个话题——**ExPAND 是目前本仓里唯一把慢层做到 SSD/SCM 且明确处理拓扑延迟的一篇**，对 MEXT 这类 flash-as-memory 路线的参考价值高于对 CXL-DRAM 分层路线的。
