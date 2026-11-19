# Camp 论文总结

**Performance Predictability in Heterogeneous Memory**
Jinshu Liu、Hanchen Xu、Daniel S. Berger、Marcos K. Aguilera、Huaicheng Li（Virginia Tech / Microsoft & UW / NVIDIA）
ASPLOS '26（Vol. 2），2026-03，DOI 10.1145/3772172.3790201；开源：`github.com/MoatLab/CAMP`

## 一句话

**不用在 CXL 上跑，就能预测在 CXL 上跑会慢多少**：把减速拆成三个有微架构因果解释的分量（需求读、预取/缓存、写缓冲），在纯 DRAM 观测上做投影——265 个负载、四种后端上 **91–97%** 的预测落在 10% 绝对误差内，总体相关性 0.97（既有代理指标只有 0.37–0.88）。

## 问题

1. **代理指标不可靠**：MPKI、带宽、平均延迟、停顿周期、AOL 这些常用指标与真实 CXL 减速的相关性只有 **0.37–0.88**。用它们做放置/交织决策，等于在错误信号上做优化。

   | 指标 | 相关性 |
   |---|---|
   | Colloid（MPKI） | 0.37 |
   | Memstrata（MPKI） | 0.40 |
   | Caption（BW+Lat+Stall+MLP） | 0.60 |
   | BATMAN（BW+Lat） | 0.66 |
   | X-Mem（BW） | 0.84 |
   | SoarAlto（AOL = Latency/MLP） | 0.88 |
   | **Camp** | **0.97** |

2. **已有系统只会事后纠正**：反应式系统（Nomad、Memtis、TPP）先迁移再纠错；描述式/归因式（Melody）要在 DRAM 与 CXL 上**各跑一次**才能解释"已经发生的减速"，无法在部署前判断。
3. **交织比例只能靠搜索**：加权交织的最优 DRAM–CXL 比例缺少闭式解，带宽受限与延迟受限负载对交织的响应**方向相反**。
4. **热度 ≠ 性能影响**：低 MPKI 负载可能在慢层上严重减速，高 MPKI 负载可能因 MLP 充足而几乎无感。

## 机制

**核心洞见**：CXL 减速的三个根源在纯 DRAM 执行时就留下"指纹"——延迟–MLP 动态、填充缓冲（LFB/SQ）压力、Store Buffer 占用。因此可以把 DRAM 观测通过传递函数**投影**成 CXL 预测。

**① 需求读减速 $S_{DRd}$**

以 memory-active cycles $C$（至少一个未完成请求的周期数）为桥：

$$S_{DRd} \approx \frac{\Delta C}{c},\qquad C = \frac{N\times L}{MLP}$$

在"跨层请求数稳定（$R_N \approx 1$，>95% 负载成立）"的前提下得到

$$S_{DRd} \approx \left(\frac{R_{Lat}}{R_{MLP}} - 1\right)\cdot\frac{C_{DRAM}}{c}$$

即**只有没被并发度吸收的那部分延迟计入减速**。$R_{Lat}/R_{MLP}$ 用双曲函数 $1/(p+q/x)$ 拟合，最终形式含 $s_{LLC}$（L3 miss stall）作代理。

论文特别澄清它与 SoarAlto 的 AOL 关系：形式相似，但 AOL 是经验相关，Camp 由 **Little 定律因果推出**——这正是 0.88 → 0.97 的来源。

**② 预取/缓存减速 $S_{Cache}$**：CXL 下 L2 预取器前瞻不足 → L1 预取绕过 L2 直接访存 → LFB 命中率上升、L1D 命中率下降 → LFB 条目被长占用并挤占需求访问。

$$S_{Cache} \approx k \cdot R_{LFB\text{-}hit} \cdot R_{Mem} \cdot \frac{s_{Cache}}{c}$$

**③ 写路径减速 $S_{Store}$**：每条 store 前必须 RFO，CXL 上 RFO 延迟约 **2–3×**，SB 条目在 RFO 完成前不能释放 → 流控失配 → SB 满后对所有后续指令（含独立读）背压。

$$S_{Store} \approx k\cdot \frac{s_{SB}}{c}$$

**④ 交织合成（MLP 不变性 + 二次延迟模型）**

关键实测：MLP 在任意交织比例下变化 **≤5%**（CXL 上 88% 负载 MLP 增幅 <10%，76% <5%）。于是问题降维成"只预测延迟"：

$$L(x') = L_{idle} + (L_{full} - L_{idle})\cdot x'^2$$

配合负载缩放因子 $M(x') = x'\cdot L(x')/L_{full}$，得到任意比例 $x$ 下的减速。高争用下三次项占主导——这在数学上解释了带宽受限负载的"浴缸曲线"。

**⑤ 决策层（Best-shot）**：把闭式模型当优化器——① 判断负载能否从聚合带宽获益；② 解析预测**执行时间最小**的交织比；③ 主动规避有害配置。不搜索、不迁移、不训练。

## 是否需要训练/开销

- **不需要 ML 训练**：全部模型由微架构推理导出，论文明确"避免黑箱 ML"。
- 一次性标定：4 个微基准（pointer chasing / sequential reads / strided / memset）标定平台常数 $p,q,k$；每平台一次。
- 运行时：**11 个 PMU 计数器**（SKX）/ 12 个（SPR/EMR）；论文只称通过 `perf` 读取"开销可忽略"，**未给量化百分比**。
- 画像次数：延迟受限负载 **1 次 DRAM run**；带宽受限负载 **2 次**（DRAM + CXL）。

## 评估设置

| 平台 | CPU | 核 | LLC | DRAM |
|---|---|---|---|---|
| SKX2S | Xeon 4110 | 10 @2.2 GHz | 14 MB | DDR4-2666，52/32 GB/s，90/140 ns |
| SPR2S | Xeon 6430 | 32 @2.1 GHz | 60 MB | DDR5-4800，191/97 GB/s，114/191 ns |
| EMR2S | Xeon 6530 | 32 @2.1 GHz | 160 MB | DDR5-4800，246/120 GB/s，111/192 ns |

CXL 设备三台：A（24 GB/s，214 ns，PCIe 5.0 ×8）、B（22 GB/s，271 ns）、C（52 GB/s，239 ns，×16）。CXL/DRAM 空载延迟比 **156%**。

基线 7 个：Interleave 1-1、Caption、First-touch、NBT（NUMA Balancing Tiering）、Colloid、Alto、Soar。负载 **265 个**（SPEC 2017、PARSEC、GAPBS、PBBS、XSBench、Phoronix + Redis/Spark/VoltDB/MLPerf/Llama/GPT-2/DLRM）；交织表征用 100+ 负载 × **101 个比例**。

## 关键数字

| 指标 | 数值 |
|---|---|
| 总体相关性 | **0.97**（NUMA 0.965 / CXL-A 0.919 / CXL-B 0.963 / CXL-C 0.940） |
| 10% 绝对误差内 | **97.3% / 94.3% / 90.7% / 96.2%**（NUMA / A / B / C） |
| 5% 绝对误差内 | 88.4% / 88.7% / 77.8% / 92.4% |
| 分量精度 | $S_{DRd}$ ≤5% 误差占 92–94%；$S_{Cache}$、$S_{Store}$ 各 93–97% |
| 交织预测 | **90%** 预测点 ≤5% 绝对减速误差（20 个带宽受限负载） |
| MLP 跨比例波动 | **≤5%** |
| Best-shot 增益 | 最高 **21%**（vs First-touch / Colloid）；单点 13%（654.roms） |
| 混排增益 | 最高 **23%**（vs 热度驱动放置）；MPKI 引导反而损失 10–12.2% |
| RFO 延迟放大 | 约 **2–3×** |

## 论文自陈的局限

1. **只在设备带宽未饱和区间成立**：饱和后延迟非线性上升会级联放大三个分量。
2. **尾延迟导致低估**：依赖 DRAM 平均延迟，对不规则访问（如 pr-twitter）在 CXL 上遇最坏尾延迟时低估惩罚。
3. **极端并行度高估**：MLP 极高时（pr-kron）高估减速。
4. **计数器盲区**：SPR/EMR 缺少"L1 预取命中 L2"的精确计数，只能用 offcore 代理近似，复杂中层缓存行为下 $S_{Cache}$ 误差升高。
5. **二次型关系不被主张普适**：*"We do not claim that latency is universally quadratic in load."*
6. **只适用于 weighted interleaving**：扩展到 first-touch 分配与迁移驱动的分层管理为未来工作。
7. **平台相关常数**需每平台标定；可移植到 AMD/ARM 的说法只是"we believe"。

## 我的判断

**它把"预测什么"这件事重新定义了一遍。** 整个分层领域（尤其 learn 系工作）默认在预测**热度**，Camp 用 265 个负载证明热度与性能影响的相关性最低到 0.37——低 MPKI 却严重减速的负载是真实存在的一类。这不是"更好的模型"，是**换了目标变量**。

**"因果结构 + 少量常数"是它的方法论选择**，与 RL 式黑箱截然相反：11 个 PMU 计数器 + 4 个微基准标定，换来跨设备泛化（CXL-C 带宽是 A 的两倍、B 延迟高 27%，仍准确）。相比之下 xTier 要用 32 个特征训 15 分钟、且绑定负载类型。两条路线的取舍很清楚：**Camp 需要先验结构，xTier 需要数据**。

**最有用的一条结论给所有做分层的人**：*"CXL slowdown can be accurately predicted without running on CXL"*。这对我们没有 CXL 硬件的处境是直接相关的——它给出了"不测也能算"的合法路径，前提是能采到同样的 PMU 计数器（并标定 $p,q,k$）。但注意它的适用范围：**设备带宽未饱和**、加权交织策略、平均延迟信号。

**与热点论文的关系**：它的基线集合（NBT/Colloid/Alto/Soar/Caption）与 TPP/TMO/OBASE/xTier 完全不重叠；唯一交叠是它把 TPP 归入"反应式"一类。它真正的前作是 **Melody**（减速三分量可加的分解）与 **SoarAlto**（AOL）——Camp 把前者从归因变成预测，把后者从相关变成因果推导。
