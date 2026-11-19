# DyNN-Offload 论文总结

**Enabling Large Dynamic Neural Network Training with Learning-based Memory Management**
Jie Ren、Dong Xu、Shuangyan Yang、Jiacheng Zhao、Zhicheng Li、Christian Navasca、Chenxi Wang、Harry Xu、Dong Li（UCAS / William & Mary / UCLA / UC Merced）
HPCA 2024（PDF 首页印 HPCA 页眉，未印年份与 DOI，年份据文件名与参考文献截止 2023）

> **层次提醒**：本篇管的是 **GPU 显存 ↔ CPU 内存**的offload，不是 CPU 侧的 DRAM/CXL 分层。列入本目录是因为它的"用 pilot model 预测张量访问时序"机制与内存分层高度同构。

## 一句话

动态神经网络（DyNN）的计算图随输入样本变化，PGO 式 offload 的预取知识无法迁移——DyNN-Offload 用 6 种 **memory idiom** 把算子归一成 9 维向量（AFM），配一个 **3,260 参数、30 µs 推理**的 pilot model 预测下一 execution block 需要的张量，在单张 GPU 上把可训练模型规模放大 **8×**。

## 问题

1. **显存容量墙**：AlphaFold 在 256 长度、128 条序列时消耗 1,024 GB；参数效率与 T5-large 相当的 switch-based MoE 至少 320 GB。
2. **PGO 假设静态计算图**：用 TreeLSTM + 6,000 样本统计控制流二值向量的 Jaccard 距离，训练样本分布为 [0,0.2] 5%、(0.2,0.4] 28%、(0.4,0.6] 25%、(0.6,0.8] 40%、(0.8,1] 2%——**按前几轮 profiling 得到的预取知识基本迁移不过去**。
3. **重计算的根本限制**：(a) 重计算可递归且深度无理论上界；(b) **常量张量与权重不可重计算**，节省上限更紧。
4. **简单启发式不足**：句子中动词/名词比例与分支决策的相关性最高仅 Spearman 0.20、Pearson 0.25；用此类启发式做预取，var-BERT 相比 DyNN-Offload 有 **50%** 性能损失，且每个 DyNN 需不同启发式。

## 机制

**① idiom 化的架构表示（AFM）**（p5）：六个访存模式——Transpose $A_{ij}=B_{ji}$、Gather $A_{ij}=BC_{ij}$、Scatter、Reduction、Stream、Stencil；论文称 PyTorch 的 300 个常用算子**全部可由这六种覆盖**。每个算子生成 **9 维向量**（6 个 idiom 计数 + 3 个张量形状维度），按程序序排列成 AFM；控制流占一行全 0；AFM 离线构建，每 DyNN 至多几 KB。

**② 输出粒度 = execution block**（p6）：每块一个 10 元素向量（算子里数 + 6 个 idiom 累计 + 3 个张量维度累计）；块 i 开始即触发块 i+1 的迁移。映射方式是从首个算子遍历静态架构、遇控制流枚举分支——**12,000 个测试用例全部精确匹配**；无匹配时退化为"算子里数最接近"的路径。块数通常 $O(10)$。

**③ 轻量 pilot model**（p6–7）：3 层输入（样本 / AFM / DyNN 基础类型 one-hot）+ **3 个并行 MLP**（各 4 层）+ 输出层，共 **3,260 个可训练参数、推理 30 µs**。用 3 个 MLP 而非单个大 MLP：单个 bulk MLP"至少慢 100×"。

**④ 标签从离线最优划分来**（p7）：把 DyNN 转静态图后用 **Sentinel**（PGO 张量卸载方法）在给定 GPU 容量下划分数据流图、最大化迁移/计算重叠，划分结果转成 10 元素向量作为标签；共 >24,000 训练样本。

**⑤ 运行时**（p7–8）：显存等分为 **work buffer / migration buffer**，块结束时角色互换；CPU 侧维护算子计数器，达到块 i 的算子数就迁块 i+2 的张量；**先 evict 再 prefetch**（并行会导致碎片化）。实现在 **ONNX Runtime** 上，只需加 2 行代码。

**⑥ 误预测处理**（p8）：误预测样本的已解析架构 + 输入被留存为未来训练样本；若新样本输出与某次误预测完全相同，直接复用该次已解析的 execution block。

## 评估设置

平台 1：4 台各 1×RTX6000（23 GB）+ 双 Xeon（24 核）+ 186 GB 内存；平台 2：2 台各 4×A100 80 GB + 双 Ice Lake（40 核）+ 500 GB。互连均为 16-lane **PCIe 3.0**。基线：**DTR**（重计算）、**UVM**（未启用 prefetch，最大 oversubscription 2）、**ZeRO-Offload**（仅静态模型），参照未修改 PyTorch。负载 8 个：Tree-CNN、UGAN、Tree-LSTM、var-LSTM、var-BERT、AlphaFold、fixed-LSTM、fixed-BERT（后两者为静态 NN 对照）。

## 关键数字

| 指标 | 数值 | 对比 |
|---|---|---|
| 单 GPU 可训练规模 | **1,500 层 / 31.25B 参数** | PyTorch 192 层 / 3.85B（**8×**） |
| 宽模型 | 64 层 / 20.2B 参数 | PyTorch 10 层 / 3.2B（6.3×） |
| 最大 batch size | 1.17× / 1.7× / **3.6×** | UVM / DTR / PyTorch（同样 200% 开销约束下） |
| AlphaFold batch | 3× / 2.1× | UVM / DTR |
| 训练时间 | −35%（最高 1.38×） | UVM / DTR |
| 静态 NN | −33% | ZeRO-Offload |
| 时间分解 | Tree-CNN/UGAN 中迁移占 55%/40% | — |
| pilot model | 3,260 参数、30 µs 推理、映射 10–15 µs；3,000 样本中 <60 次误预测 | — |
| 表示效率 | AFM 比 global-id 表示精度高至少 19%；达 0.88 精度需 2,048 vs 4M 神经元（推理 ×128、训练 ×2.4） | — |

## 论文自陈的局限

无独立 Limitations 章节，逐条摘录：

1. **UVM 基线未启用预取**（因为 DyNN 的动态性使预取知识无法先验获得）——基线设置对 UVM 不利。
2. **AFM 不显式表达张量与算子的关联细节**。
3. **必须用 3 个 MLP**：多于 3 个收敛慢且不提精度、少于 3 个掉精度。
4. **双缓冲等分**是为简化管理。
5. **新类型 DyNN 需重训**（可增量训练），且不做逐模型微调（易灾难性遗忘）。
6. **存在非精确匹配的退化路径**（按算子里数取最近）。
7. **evict 与 prefetch 不能并行**（会导致显存碎片）。

## 我的判断

**它最有价值的一点是"把预测的输入定成架构的语法"**：不是预测"哪块内存会被访问"（不可知），而是预测"这段代码会进入哪个 execution block"（由 DSL 的 idiom 组合决定）。这一步把开放式的预测问题变成了有限枚举的映射问题——12,000 个测试用例全部精确匹配就是这个设计的证据。

**"30 µs、3,260 参数"是刻意的工程约束**：DyNN 单迭代约 $O(100\,\mu s)$，预测器不能比被预测的对象更贵。这与 ReStore 的"4 个可训练参数"、IDT 的"三个分位里选一个"是同一族思路——**在线的机器学习必须小到可以与主计算同尺度**。

**层次差异必须说清**：它管 GPU 显存，不管 CPU 内存分层；本仓八篇分层论文的"迁移"是页迁移（19 µs 级），它的是张量迁移（过 PCIe，毫秒级）。但两者的结构性洞见相同：**当访问模式由计算图决定时，预测"代码要干什么"比预测"数据何时被访问"更容易**。这是我把它放进本目录的唯一理由。
