# AMD MEXT 技术洞察报告

**日期：** 2026-11-17  
**分析对象：** AMD MEXT Predictive Memory（预测性内存软件）  
**资料基础：** 本目录下 5 份 AMD 官方文档 + 2 篇学术论文 + 公开网络信息（详见文末来源清单）

---

## 一、背景与目标

### 1.1 市场背景：DRAM 成为数据中心的核心成本瓶颈

AMD 在多份文档中反复强调一个判断：**DRAM 已成为数据中心最昂贵的组件**（`papers/AMD-2026-MEXT-WhitePaper-PrimeTime-6thGen-EPYC.pdf`，p.4）。这一判断在 2026 年的行业环境中得到了外部印证：

- CounterPoint Research 的内存价格跟踪报告显示 DRAM 单位成本持续攀升（白皮书尾注 2 引用，2026 年 2 月报告）；
- 媒体报道：64GB DDR5 服务器内存条从 2025 年中的 $255 涨至 2026 年 Q1 的 $900 以上（byteiota.com 报道，2026-11）；
- TrendForce 预测 2026 年 Q4 传统 DRAM 合约价将环比再涨 10–15%（AMD IT 案例研究引用）。

同时，AMD 指出 DRAM 使用存在四大痛点（两份 PrimeTime 白皮书 p.4，表述一致）：

1. **昂贵且持续涨价**；
2. **容量难以预估**——预测不准会导致 OOM 和作业失败，因此团队普遍过度配置以规避"内存悬崖"；
3. **采购粒度粗糙**——大容量内存系统的可选容量档位粗，无法"刚好+10%"地精细配置；
4. **利用率低下**——即使配置正确，利用率也常常只有 50% 甚至更低。AMD 引用 Meta 论文（dl.acm.org/doi/10.1145/3746651，即 TMO 论文）佐证这一数字。

### 1.2 MEXT 的定位与目标

根据 `papers/AMD-2026-MEXT-Product-Brief.pdf`，MEXT 的三大设计要务是：

1. **解决 DRAM 利用率不足问题**；
2. **以最小硬/软件改动实现——做"即插即用"方案**；
3. **把闪存（flash）引入内存域**。

MEXT 于 2026 年 6 月被 AMD 收购（casrai.org、AMD IT 案例研究），2026 年 11 月 10 日在 EPYC Venice 活动上正式随第六代 EPYC 平台推广（casrai.org 报道）。其核心主张：通过软件让 NVMe 闪存"看起来像 DRAM"，让客户**用更少的物理 DRAM 跑同样的负载（降本），或在既有硬件上扩展可用内存容量（扩容）**。

> **事实与推测的区分：** DRAM 涨价、利用率低是可核实的事实；"冷页占比 50%"这一数字部分来自 Meta TMO 论文，其适用范围（数据中心普遍情况）是 AMD 的引用性推断。

---

## 二、MEXT 技术解析

### 2.1 核心机制：冷页卸载 + AI 预测性回流

MEXT 的工作流程在产品简介和三份白皮书中描述一致，可归纳为三步（`papers/AMD-2026-MEXT-Product-Brief.pdf`，"How It Works"）：

1. **冷页卸载（Cold Page Offload）**：轻量级 Linux 驱动持续采集内存页访问遥测数据，识别热页与冷页，将冷页卸载到专用 NVMe 闪存；
2. **AI 驱动预测**：AI 引擎持续预测哪些已卸载到闪存的页即将被应用再次访问（从冷变热）；
3. **预测性回流（Push Back to DRAM）**：在应用真正请求之前，透明地把这些页提前搬回 DRAM，使工作集始终驻留 DRAM。

从应用视角看，"相关内存页永远已经在 DRAM 里"，从而用预测掩盖闪存的高延迟。这与语言模型预测下一个 token 的思路同源——MEXT 的 AI 引擎预测的是**内存页访问序列**（`papers/AMD-2026-MEXT-WhitePaper-PrimeTime-6thGen-EPYC.pdf`，p.6）。

### 2.2 关键组件

安装指南（`papers/AMD-2026-MEXT-Installation-Guide.pdf`）与白皮书列出的组件略有差异，合并整理如下：

| 组件                     | 形态                                     | 职责                                                                   |
| ---------------------- | -------------------------------------- | -------------------------------------------------------------------- |
| **MEXT Driver**        | 可动态加载的 Linux 内核模块（不改内核源码，安装时按宿主内核版本编译） | 采集进程与内存页遥测数据，提供给 AI 引擎                                               |
| **MEXT AI Engine**     | 用户态进程，单核 CPU 运行，无需 GPU，推理耗时"不到一秒的一小部分" | 预测哪些页应从闪存推回 DRAM；模型族（model family/ensemble）自适应选择                     |
| **MEXT Daemon（mextd）** | Linux 系统服务                             | 执行实际的页迁移（依据 AI 引擎的预测）                                                |
| **RAM View**           | 观测/可视化工具                               | 展示实时内存利用率与页温度分布（<15s / <1min / <2min / <3min / <5min / >5min 前访问的比例） |

关于 AI 引擎的技术细节（白皮书 p.6，三份白皮书表述一致）：

- 是**模型集成（ensemble/family）**：从"极轻量启发式预测器"到"更强大的神经网络模型"（产品简介提到上限是 transformer 级模型）；
- **自动选择**：针对具体负载自动切换到表现最好的模型或模型组合；
- **在线反馈闭环**：持续观察"预测的页是否真的被应用使用"，实时校准模型精度，支持持续自适应与自优化。

> **待验证：** 白皮书未公开模型架构、训练数据规模、页粒度（推测为 4KB，未明示）、预测命中率等关键指标。AI 引擎的具体实现属于闭源黑箱，外部无法复核"预测"相对于"按访问频率被动换页"的增益大小。

### 2.3 部署形态与系统要求

来自安装指南（V1.2，2026-10-08）的**事实性**约束：

- **硬件**：8+ 物理核；EPYC 7003 及以上 / Threadripper 5000 及以上（要求 AVX 与 constant_tsc CPU flag）；DRAM ≥ 64GB；/opt 分区 120MB 空间；**一块专用的、未挂载的 NVMe SSD**（须以裸设备如 /dev/nvme1n1 暴露给 OS）；
- **SSD 建议**：PCIe Gen4+；容量 ≥ 期望 MEXT 内存的 1.75 倍（最低）/2 倍以上（推荐）；服务器推荐 3DWPD 企业级盘（工作站最低 1DWPD）；
- **OS/内核**：RHEL 8.6–10.1、Oracle Linux（仅 RHCK 内核）、SLES 15 SP4/SP6-SP7、Ubuntu 22.04/24.04/26.04，各有最低内核版本要求；
- **限制条件**：**必须关闭 Secure Boot**（可能阻止所需内核功能）；**系统不得配置磁盘 swap 设备**；安装配置需 root，日常使用不需要；
- **资源占用**：产品简介称占用"两个 CPU 核"（AI 引擎 + daemon 各一）；中国媒体报道口径为"仅需一颗 EPYC 核心完成本地训练与推理"（chinaaet.com）——两者略有不一致，需以官方 FAQ 为准（casrai.org 转述 FAQ 称两个核）；
- **部署范围**：裸机、虚机（部署在 Guest OS 层而非 Hypervisor 层）、容器环境均可，云上/本地均可，声称"任何处理器"都能用（安装指南 Ch.1），但测试数据全部来自 EPYC 平台；
- **默认配置**：安装后默认提供相当于物理 DRAM 20% 的 MEXT 内存，可通过 `mext-config --set-config mextMemory=40%` 修改并重启生效；
- **运维特性**：mextd 服务**不允许运行中 stop**——"等价于在运行中的系统上拔内存条"，必须 disable 后重启。

### 2.4 性能数据（AMD 内部测试，均为厂商自报）

**测试 1：EDA 静态时序分析（PrimeTime），EPYC 9686F，512GB 基线**（`papers/AMD-2026-MEXT-WhitePaper-PrimeTime-6thGen-EPYC.pdf`，2026-10/11）：

| 配置                       | 平均耗时    | 相对性能  | 性能/DRAM-美元 |
| ------------------------ | ------- | ----- | ---------- |
| 512GB 纯 DRAM 基线          | 11,407s | 100%  | 1x         |
| 256GB DRAM + ~256GB MEXT | 12,031s | 94.8% | ~1.90x     |
| 192GB DRAM + ~320GB MEXT | 12,662s | 90.1% | ~2.40x     |
| 128GB DRAM + ~378GB MEXT | 14,173s | 80.5% | ~3.22x     |

**测试 2：PrimeTime 大规模场景，EPYC 9755，1.5TB 基线**（`papers/AMD-2026-MEXT-WhitePaper-PrimeTime-1.5TB.pdf`，2026-10）：

- 768GB DRAM + 768GB MEXT（总量回补到 1.5TB）：耗时约 112,000s vs 基线约 98,000s，**保留约 87% 性能，性能/DRAM-美元 1.74x**；
- 同样 50% DRAM 削减但**不开 MEXT**：直接 OOM（无内存安全网时）。

**测试 3：Agentic AI 基准，双路 EPYC 9996，~1TB/路 基线**（`papers/AMD-2026-MEXT-WhitePaper-AgenticAI-6thGen-EPYC.pdf`，2026-11）：

- 50% DRAM + 512GB MEXT：五角色混合阶段几何平均吞吐 3.921 vs 基线 3.910 tasks/s（**99.7%**），性能/DRAM-美元 ~1.99x；单独的内存密集型图像处理 agent（mtpillow）为 89.7%（1.79x）；
- 25% DRAM + 768GB MEXT：混合几何平均 85.3%（3.41x），mtpillow 88.4%（3.54x）。

**生产部署佐证**（AMD IT 案例研究 + byteiota 报道）：AMD 自身 IT 部门 2026 年 9 月起在 86 台（后扩至 145 台）生产服务器部署 MEXT，提供约 58TB 额外内存，**规避了 82–83% 的 DRAM 扩容成本**（约 $4.9M 采购节省），计划 2027 年底扩至 2000 台。

> **口径提醒（重要）：** "性能/DRAM-美元"指标仅把性能按"保留的 DRAM 比例"归一化，**不计入 NVMe 成本、MEXT 许可费、其他系统成本与 TCO**——AMD 自己在尾注中明确声明了这一点。Phase 2 将转为商业授权模式，价格未公布（byteiota.com）；若授权费接近省下的 DRAM 差价，经济账会完全改写。

---

## 三、业界实践对比

MEXT 处在一个正在快速成形的"内存分层/扩展"技术版图中。按"慢层介质 + 管理机制"两个维度，可把业界路线归为四类：

### 3.1 路线 A：内核透明换页（swap/offload）——Linux 原生 + Meta TMO

**代表：** Linux 内核 swap/zswap、Meta TMO + PSI/Senpai、Google zswap（g-swap）。

- Meta TMO（CACM 68(9), 2025，即白皮书引用的 dl.acm.org/10.1145/3746651）：内核驱动的透明内存卸载，PSI 实时度量进程因资源不足的停顿，用户态 agent Senpai 据此动态决定卸载量，卸载目标包括压缩内存与 NVMe SSD。**2021 年起在 Meta 数百万台服务器生产运行，节省 20–32% 总内存**，已上游进 Linux 内核。
- TPP（Transparent Page Placement，Meta，eEuroSys'23）：把冷页降到 CXL/远端 NUMA 层、热页自动回升的 Linux 机制。

**与 MEXT 的对比：** TMO/TPP 是"被动响应式"——先检测到压力/冷热，再行动；且已开源上游。MEXT 的差异点在**预测性**（提前把页搬回，而不是等缺页中断）与**商业化独立软件**（闭源、按license收费、不依赖内核新机制、Secure Boot 需关闭）。TMO 的卸载目标以压缩内存+zswap 为主，MEXT 则把 NVMe 直接做成"内存设备"。二者解决的是同一个经济学问题，但 MEXT 的卖点是"无需上游内核特性、单一供应商支持、EDA/Agentic AI 等垂直负载验证数据"。

### 3.2 路线 B：CXL 内存扩展与分层——硬件总线路线

**代表：** CXL 1.1/2.0/3.x 内存扩展器与池化、Meta Vistara ASIC、学术界 TPP/Memtis/Colloid/NOMAD/Equilibria/xTier 等。

- **Meta Vistara**（2026-07 报道）：自研 CXL 芯片把退役服务器的 DDR4 桥接到新 DDR5 服务器（每芯片 256GB DDR4-3200，PCIe Gen5），基于 Linux TPP/TMO 做热页留本地、冷页下沉 CXL 的透明分层。生产部署于百万级服务器，推理服务器数量最多减 25%，缓存延迟降 29%，OOM 失败率降 33%。CXL 扩展功耗仅约 50W，空闲延迟约 250ns、峰值带宽约 48GB/s——但生产中 CXL 带宽利用率常低于 10%，因为冷数据才放 CXL。
- **学术前沿**（本目录两篇论文 + 检索结果）：
  - **Camp**（`papers/Liu-2026-ASPLOS-Performance Predictability in Heterogeneous Memory.pdf`，ASPLOS'26）：证明 MPKI、带宽、延迟、停顿周期等**单一指标与 CXL 减速的相关性只有 0.37–0.88**，而基于 12 个 PMU 计数器的因果解析模型可达 0.97；并给出 DRAM–CXL 任意交织比例的闭式性能预测（"Best-shot"策略最多比现有 tiering 提升性能 21%）。**关键启示：页放置决策的"预测"问题有严格的可解结构，热度≠性能影响。**
  - **Equilibria**（Meta，arXiv 2602.08800）：指出 TPP 之外的 tiering 系统（Memtis、Colloid、NOMAD、HeMem 等）几乎都未上游内核，且普遍不支持多租户公平性；
  - **xTier**（arXiv 2609.27266）：用 eBPF 挂载 PEBS 事件 + INT8 量化 MLP **在内核态做微秒级页排序推理**，稳态占用不足 1 核的 4%，在 DRAM:CXL=1:15 以下的紧配置中 18 种配置里 14 种最快；
  - **Memtis**（TOCS 2026）：利用页访问频率直方图做快速层容量动态再分配；
  - CyberLeninka 综述指出行业趋势是**从"频繁迁移"转向"选择性迁移"**（如 PACT 以最多 50 倍更少的迁移达到最高 +61% 性能）。

**与 MEXT 的对比：** CXL 路线是硬件标准化路线，延迟增量约 70–100ns（比 NAND 低 2–3 个数量级），但需要新硬件采购与生态成熟；MEXT 用现有 NVMe 盘 + 软件即可运行，闪存粒度延迟（几十微秒级）比 CXL 慢得多，**完全依赖预测精度来弥合**。CXL 路线的页放置研究（Camp/SoarAlto 等）同样在向"预测式/性能建模式"管理演进——MEXT 的 AI 集成思路与学术方向一致，但其黑箱程度更高（Camp 明确反对黑箱 ML，主张可解释因果模型）。

### 3.3 路线 C：闪存/新介质直接入内存域——MEXT 所在的路线

**代表：** AMD MEXT、NVIDIA CMX（专面向 LLM 推理 KV cache）、Kioxia XL-Flash / Samsung Z-NAND（SCM 残余路线）、Intel Optane（已故）。

- **NVIDIA CMX**（GTC 2026 发布）：在 G1（GPU HBM）/G2（主机内存）与 G3（本地 SSD）/G4（企业存储）之间新增 **G3.5 层——以太网挂接的 pod 级共享闪存池**，专为 KV cache 优化。BlueField-4 STX 存储处理器 + Spectrum-X RDMA + DOCA Memos + Dynamo/NIXL 服务集成，宣称长上下文推理最高 5x token/s 与 5x 能效。设计哲学与 MEXT 相同——**为可重建的暂态数据（KV cache/冷页）建立专用闪存层，不做持久化存储的过度保护**——但 CMX 是应用感知（KV block 级、Dynamo 调度）、网络共享、硬件协同设计；MEXT 是应用透明（页级、单机、纯软件）。
- **Optane 教训**（The Register, 2026-07 回顾）：Optane 的低延迟与超高写寿命本可完美匹配 KV cache 型负载，但因密度 Scaling 失败于 2022 年停产。这提&#x793A;**"新介质"路线的商业风险远大于"软件定义分层"**——MEXT 恰好选择了介质风险最小的路径（普通 NVMe）。

### 3.4 路线 D：ML 驱动的缓存/分层策略——方法论背景

`papers/Savva-2026-ACMCSUR-Machine Learning-Based Caching and Tiering in Modern Data Storage Systems - A Survey.pdf`（ACM CSUR 58(11), 2026）系统梳理了 ML 在缓存淘汰/准入/预取与分层放置/迁移中的应用，关键结论：

- **三类范式**：离线训练+在线推理、增量在线训练、RL 在线训练；特征以 recency/frequency/最后访问时间/访问序列为主；
- **DL（尤其 LSTM/Transformer）主导预取**——因为它最擅长捕捉时间/空间相关性，这**在方法论上直接支撑 MEXT "预测页访问序列"的技术选择**；
- **共性缺陷**（对 MEXT 同样适用）：模型计算复杂度高、需要大数据训练、**很多工作只在模拟器上验证过**（Table 8）、对未见负载的适应性存疑；
- 趋势判断：RL 适合快速自适应的淘汰决策，DL 适合预取与放置，整体向"更复杂 ML 模型管理更智能分层"演进。

### 3.5 横向对比总表

| 维度     | AMD MEXT              | Meta TMO/TPP   | Meta Vistara(CXL)  | NVIDIA CMX       | Linux 原生(NBT/AutoNUMA) |
| ------ | --------------------- | -------------- | ------------------ | ---------------- | ---------------------- |
| 慢层介质   | NVMe SSD（专用裸盘）        | 压缩内存/zswap/SSD | 旧 DDR4（CXL 桥接）     | 以太网闪存池           | 任意 NUMA/CXL 节点         |
| 管理粒度   | 内存页（透明）               | 内存页（透明）        | 内存页（透明，TPP驱动）      | KV block（应用感知）   | 内存页（透明）                |
| 决策机制   | AI 模型集成预测（黑箱）         | PSI 停顿度量（反馈式）  | 冷热统计（反馈式）          | Dynamo 调度+预取     | 热度统计（反馈式）              |
| 开放性    | 闭源商业软件（Phase 2 收费）    | 已上游 Linux      | 自研 ASIC+内核机制       | 闭源平台（硬件绑定）       | 内核原生                   |
| 硬件要求   | 现有服务器+一块 NVMe         | 现有硬件           | 新自研 CXL 芯片         | BlueField-4+整套网络 | 支持 CXL/NUMA 的平台        |
| 部署规模证据 | AMD 自身 145 台          | Meta 数百万台      | Meta 百万台级          | 尚在发布期            | 主流发行版默认                |
| 延迟代价   | 预测失误→几十微秒级闪存访问        | 换页开销           | +120ns 左右（250-130） | 网络闪存，微秒级         | 迁移开销                   |
| 主要场景   | EDA、Agentic AI、数据库、媒体 | 数据中心通用         | 推理/缓存/大数据          | LLM 长上下文推理       | 通用                     |

---

## 四、趋势判断

综合资料与调研，给出以下判断（**均为基于证据的推测，非事实**）：

1. **"软件定义内存"窗口期正在打开。** DRAM 涨价周期（机构普遍预计短缺延续到 2027–2028）与 CXL 生态成熟之间的时间差，正是 MEXT 这类纯软件方案的价值窗口。TrendForce 等分析认为 AMD 正是拿 MEXT 对冲 Intel 的 CXL 叙事（byteiota.com 转述）。
2. **页级"预测式"管理成为学术与产业共识方向。** Camp 证明可解释因果模型能高精度预测放置影响；xTier 把 ML 推理搬进内核；SoarAlto 用 AOL（延迟/MLP）指标；MEXT 用模型集成预测序列——殊途同归：**从"检测热度"走向"预测性能影响"**。MEXT 是这一方向上商业化最快的成品之一。
3. **Agentic AI 是内存分层的新增长场景。** MEXT 白皮书专门为 agentic 负载做了测试（五角色并发），NVIDIA CMX 直接为 KV cache 设计 G3.5 层——两大厂商都把"Agent 工作流的内存 footprint 增长"视为分层技术的核心驱动。
4. **闭源 vs 上游内核的生态之争将决定长期格局。** Meta 系（TMO/TPP/Equilibria/Vistara）全部走"上游 Linux + 开放生态"路线，AMD MEXT 走闭源商业路线。历史上（Optane、g-swap 的对比）规模化最终依赖开放生态；但 MEXT 的差异化恰恰在于免内核改造、垂直负载调优。两种模式可能长期并存：**超大规模自建（Meta/Google 模式）vs 企业外购（MEXT 模式）**。
5. **商业定价是最大不确定变量。** Phase 1 免费推广、Phase 2 转授权收费且价格未公布——若定价接近 DRAM 差价，经济学逻辑崩塌；这是所有评估者应盯住的第一指标。

---

## 五、风险与局限

### 5.1 技术层面

- **延迟悬崖依赖预测精度**：NVMe 访问延迟是 DRAM 的百倍量级，预测未命中时性能惩罚远大于 CXL 路线（+70–100ns）。AMD 数据显示 50% DRAM 削减时性能保留 87–99%，但 75% 削减时已降至 80–85%——**边际递减明显，且对不同负载差异大**（CASRAI 亦提醒性能证据全部来自 AMD 自选负载）。
- **写寿命与冷数据假设**：内存页持续下卸/回流意味着 NVMe 承担写密集负载。安装指南推荐 3DWPD 企业级盘暗示了磨损问题，但**未公布写放大系数与盘寿命影响数据（待验证）**。
- **黑箱 AI 的可解释性与一致性**：与 Camp 所代表的"可解释因果建模"路线相反，MEXT 的模型族对外不透明，预测失败模式、冷启动时间、模型漂移处理均无公开数据。
- **每机两核开销 + 独占 NVMe 盘**：小集群或核数紧张场景的隐性成本。
- **安全与合规约束**：必须关闭 Secure Boot、禁止 swap 配置——这在安全基线严格的云端/企业环境中可能是直接否决项。
- **单机方案**：无跨节点共享/池化能力，与 CMX 的 pod 级共享、Vistara 的池化路线相比，在大规模推理集群场景缺乏伸缩性。


### 5.2 证据层面

- **全部性能数据为 AMD 内部自报**，无独立第三方复现（CASRAI、TechMoon 对 NVIDIA 数字也提出同样批评）；
- "性能/DRAM-美元"指标**剔除了 NVMe、许可费与 TCO**（AMD 尾注自认）；
- Agentic AI 测试使用 AMD 自研基准 kit（agentic-saturation-replay 0.3.0-pre），非行业标准负载；
- 对照组（纯 DRAM 减半）样本仅 3 次平均，未报告方差。

### 5.3 生态层面

- 闭源 + 销售渠道获取（无公开下载），社区无法审计与二次开发；
- 依赖内核模块与特定 OS 版本矩阵，长期维护与内核升级兼容性存疑；
- AMD 声称"任何处理器可用"，但全部验证在 EPYC 上——跨平台（Intel/ARM）效果**待验证**。

---

## 六、结论与启示

1. **MEXT 的本质是用"预测精度"换"内存成本"**：它赌的是 AI 序列预测能力足以把 NVMe 的百倍延迟差掩盖到工作集粒度。在 EDA（工作集大但温度分布清晰）与 agentic AI（吞吐型而非尾延迟型）这两类负载上，AMD 的自报数据显示这个赌注初步成立；对延迟敏感、访问模式不可预测的负载则证据不足。
2. **它填补的是一个时间窗，而非终局。** DRAM 涨价 + CXL 未熟的 2026–2028 窗口期内，纯软件闪存分层是最务实的过渡方案；中长期 CXL 池化（Vistara 模式）与应用感知分层（CMX 模式）各自占据更优的物理层位置。三者更可能互补（MEXT 可作为 CXL 时代的策略层）而非互斥。
3. **对评估者的可操作建议**：（a）以 POC 实测自家负载在 50%/75% DRAM 削减下的性能曲线，不采信跨负载外推；（b）把 Phase 2 许可价格纳入 TCO 才能得出真实经济性；（c）确认 Secure Boot/swap 约束与安全基线兼容；（d）关注 NVMe 磨损指标（DWPD 消耗速率）。
4. **对本目录学术资料的定位**：Liu 的 Camp 论文说明"预测式页管理"有严格理论基础（相关系数 0.97 vs 启发式 0.37–0.88），反衬 MEXT 黑箱模型的验证缺口；Savva 综述说明 DL 序列预测用于预取是学界成熟方向，MEXT 的技术选择不孤立，但综述同时指出的"多在模拟器验证、计算开销高"等通病，正是 MEXT 这类产品化尝试需要用生产数据回答的问题。
5. **更大的行业信号**：AMD（软件）、Meta（内核+自研 ASIC）、NVIDIA（硬件协同设计）在 2026 年同时重兵投入内存分层，说明**内存已成为 AI 基础设施中与算力同等级的瓶颈变量**。无论 MEXT 本身成败，"把闪存纳入内存域 + 智能放置"都会是未来 5 年服务器体系结构的持续性主题。

---

## 附录：资料来源清单

**本地文档（/Users/bbchen/workspace/research-mext/）：**

1. `papers/AMD-2026-MEXT-Product-Brief.pdf` — MEXT 产品简介（目标、组件、工作原理、PrimeTime 1.5TB 结果摘要）
2. `papers/AMD-2026-MEXT-WhitePaper-PrimeTime-6thGen-EPYC.pdf` — PrimeTime 白皮书，EPYC 9686F，512GB 基线（2026-11）
3. `papers/AMD-2026-MEXT-WhitePaper-PrimeTime-1.5TB.pdf` — PrimeTime 白皮书，EPYC 9755，1.5TB 基线（2026-10）
4. `papers/AMD-2026-MEXT-WhitePaper-AgenticAI-6thGen-EPYC.pdf` — Agentic AI 白皮书，EPYC 9996（2026-11）
5. `papers/AMD-2026-MEXT-Installation-Guide.pdf` — 安装指南 V1.2（2026-10-08，系统要求/组件/配置/服务管理）
6. `papers/Liu-2026-ASPLOS-Performance Predictability in Heterogeneous Memory.pdf` — Camp 框架，ASPLOS'26（异构内存性能可预测性）
7. `papers/Savva-2026-ACMCSUR-Machine Learning-Based Caching and Tiering in Modern Data Storage Systems - A Survey.pdf` — ACM CSUR 58(11), 2026（ML 缓存/分层综述）

**外部来源（2026-11-17 检索）：**  
8\. AMD IT 案例研究：amd.com/en/resources/case-studies/amd-it-avoids-82-of-dram-expansion-costs.html（86→145 台部署，82% 成本规避）  
9\. CASRAI 新闻分析：casrai.org/news/amd-mext-predictive-memory-software-november-2026（发布时间线、访问渠道、FAQ 转述）  
10\. byteiota.com 分析（2026-11）：DRAM 价格数据、$4.9M 节省、HPE 合作、Phase 2 授权、竞争格局  
11\. chinaaet.com / zol.com.cn（2026-11-12）：中文媒体报道（"单核运行"口径、20–75% DRAM 削减主张）  
12\. Meta TMO 论文：dl.acm.org/doi/10.1145/3746651（CACM 68(9)，2025；生产环境 20–32% 内存节省）  
13\. Meta Vistara 报道：腾讯新闻（2026-07-02）与微信公众号"EEPW 芯视角"（CXL DDR4 复用、TPP/TMO 分层、生产数据）  
14\. NVIDIA CMX：nvidia.com/en-eu/data-center/ai-storage/cmx/ 及 nvda.ws/4zuWhz6（G3.5 层、BlueField-4、5x 宣称）  
15\. Equilibria（arXiv 2602.08800）、xTier（arXiv 2609.27266）、Memtis（ACM TOCS，doi 10.1145/3777549）：CXL tiering 学术系统对比  
16\. The Register（2026-07-29）：Optane 回顾与 KV cache 写寿命讨论  
17\. TechnoLynx：TPP 18% 提升、CXL +50–100ns、TPP 论文"55–80% 内存两分钟内空闲"等数据整理

**事实/推测/待验证标注约定：** 第二、三章标注页码的描述为文档事实；第四章为推测性判断；各处"待验证"标记为当前资料无法确认的问题。
