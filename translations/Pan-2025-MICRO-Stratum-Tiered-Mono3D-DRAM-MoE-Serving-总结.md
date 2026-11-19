# Stratum 论文总结

**Stratum: System-Hardware Co-Design with Tiered Monolithic 3D-Stackable DRAM for Efficient MoE Serving**
Yue Pan、Zihan Xia、Lanxiang Hu、Tajana Rosing、Mingu Kang（UCSD）、Po-Kai Hsu、Janak Sharda、Shimeng Yu（Georgia Tech）、Hyungyo Kim、Nam Sung Kim（UIUC）、Minxuan Zhou（IIT）
MICRO '25，DOI 10.1145/3725843.3756043；arXiv:2510.05245

## 一句话

MoE 的专家权重占了模型 95% 以上，而 HBM 的 TSV 间距（~10 µm）限制了带宽——Stratum 用 **Mono3D DRAM**（Cu–Cu 混合键合 1 µm，带宽/密度提升一个量级）把**层数变成内存层级**：WL 阶梯走线使每层 tRCD 不同（2.29 → 22.88 ns），据此做 **8 级 tiering**，再用轻量主题分类器把"当前话题偏好的专家"预放到快层——解码吞吐比 GPU 基线高 **8.29×**，能效高 **7.66×**。

## 问题

1. **MoE 体量与带宽墙**：专家权重占模型绝大部分（Mixtral 8×7B 中 >95%），HBM 经硅中介层带宽不足，LLM 解码是 memory-bound。
2. **HBM 的 TSV 间距 ~10 µm** 既占面积又限带宽；把计算放进 DRAM die（PIM）则受 DRAM 工艺的 PPA 与热开销制约。
3. **Mono3D 垂直扩展带来层间延迟不均**：WL 阶梯走线使底层 RC 延迟显著增大，数百层时差异巨大；按最坏情况设计会浪费内部带宽。
4. **专家激活随查询主题高度偏斜**：LLaMA-4 Scout 在数学/逻辑类 MMLU 子集上呈现 >90% 的领域专家亲和性。

## 机制

**① 系统集成**：DRAM die 与 logic die 以 Cu–Cu 混合键合（**1 µm**，比 HBM 的 10 µm TSV 细约 5×），堆栈再经 2.5D 硅中介层连 xPU，外部接口沿用 HBM3（1024b、6.4 Gbps/pin）。三种配置：**Stratum-S**（32 GB + RTX A6000 级 die）、**Stratum-L**（6 芯片 + H100 级 die）、**Stratum-XL**（2×Stratum-L，384 GB）。

**② In-memory tiering（关键）**：按 WL 层分 **8 个时序 tier**，tRCD = [2.29, 3.92, 5.99, 8.50, 11.44, 14.82, 18.63, **22.88**] ns，tRP = 4.77 ns，tRAS = tRCD + 27.50 ns；**最快 tier 比最慢快 1.6×**。bank 内用**可编程 tiering table** 完成 `row-addr → tierID → tRCD` 查表。

**③ NMP 微架构**：每 chip 16 个 PU，每 PU 绑一个 DRAM channel、双向 ring 互连；每 PU 含 16 个 PE，tensor core 为 **16×16 MAC**；合计 64k MAC @1 GHz = **128 TFLOPS（FP16）**，片上 SRAM 36 MB；ring 只需 128 GB/s/link 即无损。

**④ 算子映射**：专家内三个级联 GeMM；GeMM1/2 权重按列切、GeMM3 按行切——**避免沿 M 维切分导致复制专家权重**；多专家串行、所有 PU 做张量并行。注意力按 head 并行，Softmax 拆 3 步、2 轮 inter-PU 标量通信。

**⑤ 主题感知放置**：67M 参数 DistillBERT、**6 个主题**的分类器；离线统计 per-topic 专家命中率；Algorithm 1 以 τ = k·L（k 个激活专家 × L 层）为"快 tier 专家数"阈值，按频率降序排进行区间。四类数据分层：hot/cold 专家权重、KV cache（中间速率）、non-NMP 数据（最慢）。换 topic 时用 **8 KB row-swap buffer** 在近存内完成行交换，不走 host。

**⑥ 物理约束**：$P_{dram} + P_{compute} + P_{misc} \le P_{peak}$，其中 $P_{dram} = BW_{fast} \cdot E_b$、$P_{compute} = N_{mac}\cdot f\cdot E_{mac}$；据此得每芯片 logic die 功耗上限约 **45 W**、可用面积 82 mm²。

## 评估设置

器件：Mono3D DRAM **1024 层**、35 nm、BL/WL pitch 70 nm/1 µm、bank 1 Gb、row buffer 32 Kb、0.429 pJ/bit、芯片 121 mm²；8 tier × 4 GB；每芯片 16 channel × 16 bank。逻辑 die：SystemVerilog + Genus + ASAP7 7 nm，FP16。建模：自研周期级 + 系统级模拟器（Poisson 到达）。基线：**RTX A6000、2×H100、4×H100**（均 vLLM 0.8.1 throughput 模式）、**Duplex**；内部消融 no-tiering vs tiering。负载：OLMoE-1B-7B、Mixtral 8×7B、Qwen2.5-32B、Llama-4-Scout，序列长度 512–8192，batch 32/64/128。

## 关键数字

| 指标 | 数值 | 对比 |
|---|---|---|
| 平均解码吞吐 | **8.29× / 5.39× / 6.13× / 4.48×** | GPU 基线（OLMoE/Mixtral/Qwen2.5/Llama-4） |
| 能效 | **7.66× / 2.74× / 3.51× / 4.87×** | 同上 |
| tiering 相对 no-tiering | 1.45× / 1.39× / 1.32× / 1.34× | 同配置关闭 tiering |
| 相对 Duplex | 吞吐 2.9×/2.5×/3.0×/2.2×；能效 2.7×/1.9×/2.9×/2.1× | Duplex（数据外推 + 保守缩放） |
| 不同 batch | 4.7×–9.8× | GPU（Llama-4-Scout） |
| 每面积吞吐 | 1.21× / 2.96× | 256 层 / 64 层 Mono3D（归一 1024 层） |
| hot expert 命中率 | 31.6% / 48.5% / 68.9% | Mixtral / OLMoE / Llama-4 |
| topic 切换开销 | ≤0.90 ms（0.23%）、≤0.37% 吞吐、≤0.35 mJ | 每 batch 换 topic |
| 存储密度 / 带宽 | **2.156 Gb/mm²（5.2×）** / 19.01–30.34 TB/s | 32 Gb DDR5 die（0.417 Gb/mm²） |

## 论文自陈的局限

无独立 Limitations 章节，逐条摘录：

1. **1024 层是假设**：*"we assume up to 1024 wordline (WL) stacks to reflect the near-future feasibility."*
2. **与 Duplex 的对比非实测**：*"we also extracted data from the previous work Duplex and made conservative scaling."*
3. **GPU 能耗是工具读数**（NVIDIA-SMI），非板级实测。
4. **45 W 上限依赖散热假设**（引用"3D IC 冷却可管理 200 W/cm²"）。
5. **6 类主题只覆盖 93% 查询**，余 7% 未展开。
6. **hot expert 命中率偏低**，Mixtral 仅 31.6%。
7. **全篇无流片/实测**：器件用 Coventor + NeuroSim、电路用 Genus 综合、系统用自研模拟器。

## 我的判断

**它的核心创意是"把工艺缺陷变成架构特性"**：WL 阶梯走线本来是 Mono3D 的负担（层间 RC 不均），Stratum 反过来把它读成"天然的多档延迟"，于是**层数 = 层级**，无需额外硬件就能得到 1.6× 的延迟梯度。这是本仓所有论文里唯一一个"层级由三维几何天然产生"的设计。

**tiering 的收益（1.32–1.45×）远小于相对 GPU 的总收益（4.5–8.3×）**——这说明它的主要优势来自 HBM 之外的内存形态（带宽与密度 5.2×），tiering 是锦上添花。读这篇时不要误把 8.29× 归因于分层。

**它与本仓八篇内存分层论文无引用关系**（92 条参考文献中无 TPP/TMO/Memtis/OBASE/xTier/Camp/Equilibria/Harmonia）。但方法上高度可比：**Stratum 的"主题预测"与 MANTA 的"未来访问预测"是同一类前馈思路，只是预测对象从"页"换成了"专家"**；而它的 6 类粗粒度主题（覆盖 93%）与 MANTA 的每应用一模型形成鲜明对照——**粗粒度预测 + 硬件分层，精度不够但够快**。
