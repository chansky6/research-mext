# 论文索引与阅读地图

本目录存放**中文总结**（每篇一份，两页速览：问题/机制/数字/局限/判断）与**完整翻译**（部分论文）。PDF 全部在 `../papers/`。

**命名约定**：`<第一作者>-<年份>-<会议>-<主题>-总结.md`；`-完整翻译.md` = 全文逐节翻译。

**使用建议**：先读总结的"一句话"与"关键数字"；需要复现机制或引用原文时，再翻完整翻译或原 PDF。总结里的数字均带页码或章节锚点。

---

## 一、总索引（18 篇）

### A. 内存分层与 CXL（核心）

| # | 论文 | 年份·会议 | 一句话 | 总结 | 翻译 | PDF |
|---|---|---|---|---|---|---|
| 1 | **TMO** Transparent Memory Offloading | ASPLOS '22 | PSI 把"内存压力"换成"损失的工作量"，生产省 20–32% | [总结](Weiner-2022-ASPLOS-TMO-Transparent-Memory-Offloading-in-Datacenters-总结.md) | — | [PDF](../papers/Weiner-2022-ASPLOS-TMO-Transparent-Memory-Offloading-in-Datacenters.pdf) |
| 2 | **TPP** Transparent Page Placement | ASPLOS '23 | CXL 当可降级 NUMA 节点而非 swap，达理想基线 <1%，已进内核 v5.18 | [总结](Maruf-2023-ASPLOS-TPP-Transparent-Page-Placement-for-CXL-Enabled-Tiered-Memory-总结.md) | — | [PDF](../papers/Maruf-2023-ASPLOS-TPP-Transparent-Page-Placement-for-CXL-Enabled-Tiered-Memory.pdf) |
| 3 | **Equilibria** Fair Multi-Tenant CXL Tiering | arXiv '26 | 每容器 protection/bound 两旋钮 + thrashing 抑制，比上游最高 +52% | [总结](Equilibria-2026-arXiv-Fair-Multi-Tenant-CXL-Memory-Tiering-总结.md) | — | [PDF](../papers/Equilibria-2026-arXiv-Fair-Multi-Tenant-CXL-Memory-Tiering.pdf) |
| 4 | **Camp** Performance Predictability | ASPLOS '26 | 不在 CXL 上跑也能预测减速，相关性 0.97（热度类仅 0.37–0.88） | [总结](Liu-2026-ASPLOS-Performance-Predictability-in-Heterogeneous-Memory-总结.md) | — | [PDF](../papers/Liu-2026-ASPLOS-Performance%20Predictability%20in%20Heterogeneous%20Memory.pdf) |
| 5 | **OBASE** Object-Based Address-Space Engineering | OSDI '26 | 先改对象布局再分层，未修改的后端收益 2–4×，代价 2–5% | [总结](Banakar-2026-OSDI-OBASE-总结.md) | [翻译](Banakar-2026-OSDI-OBASE-中文翻译.md) | [PDF](../papers/Banakar-2026-OSDI-OBASE-Object-Based-Address-Space-Engineering-to-Improve-Memory-Tiering.pdf) |
| 6 | **xTier** Intelligent Tiering for CXL | arXiv '26 | eBPF + PEBS + 34 µs MLP 进内核，14/18 最快，迁移量少 13–22× | [总结](xTier-2026-arXiv-Intelligent-Tiering-for-CXL-Enabled-Memory-总结.md) | — | [PDF](../papers/xTier-2026-arXiv-Intelligent-Tiering-for-CXL-Enabled-Memory.pdf) |
| 7 | **IDT** Intelligent Data Placement | HPDC '24 | 真实四层内存 + PPO 学各层降级阈值，2.08× vs 默认 Linux | [总结](Chang-2024-HPDC-IDT-Intelligent-Data-Placement-RL-总结.md) | — | [PDF](../papers/Chang-2024-HPDC-IDT-Intelligent-Data-Placement-RL.pdf) |
| 8 | **MANTA** ML Augmented Tiering Advisor | EuroSys '27 | 先造离线最优 ChOMP，再回归未来折扣访问次数，4 GB 下 1.12× vs ARMS | [总结](Freischuetz-2027-EuroSys-MANTA-ML-Augmented-Tiering-Advisor-总结.md) | — | [PDF](../papers/Freischuetz-2027-EuroSys-MANTA-ML-Augmented-Tiering-Advisor.pdf) |

### B. 存储层与 flash / SSD 分层

| # | 论文 | 年份·会议 | 一句话 | 总结 | 翻译 | PDF |
|---|---|---|---|---|---|---|
| 9 | **Baleen** ML Admission & Prefetching | FAST '24 | 接纳建模成 episode 级背包，峰值 DT −12%、TCO −17% | [总结](Wong-2024-FAST-Baleen-ML-Admission-and-Prefetching-for-Flash-Caches-总结.md) | — | [PDF](../papers/Wong-2024-FAST-Baleen-ML-Admission-and-Prefetching-for-Flash-Caches.pdf) |
| 10 | **ReStore** RL Data Migration | SIGMOD '26 | 每层一个 RL agent（仅 4 个可训练参数），运行时最多 −6× | [总结](Zhang-2026-SIGMOD-ReStore-RL-Data-Migration-Multi-Tiered-Storage-总结.md) | — | [PDF](../papers/Zhang-2026-SIGMOD-ReStore-RL-Data-Migration-Multi-Tiered-Storage.pdf) |
| 11 | **Harmonia** Multi-Agent RL for HSS | ICS '26 | 放置与迁移必须拆给两智能体，单智能体反而差 21.9% | [总结](Nadig-2026-ICS-Harmonia-总结.md) | [翻译](Nadig-2026-ICS-Harmonia-完整翻译.md) | [PDF](../papers/Nadig-2026-ICS-Harmonia-Enhancing-Data-Placement-and-Migration-in-Hybrid-Storage-Systems.pdf) |
| 12 | **ExPAND** CXL Topology-Aware Prefetching | IEEE Micro '25（arXiv） | 预取下放到 CXL-SSD 控制器 + 拓扑感知的"准时性"判据 | [总结](Oh-2025-IEEEMicro-ExPAND-CXL-Topology-Aware-Expander-Driven-Prefetching-总结.md) | — | [PDF](../papers/Oh-2025-IEEEMicro-ExPAND-CXL-Topology-Aware-Expander-Driven-Prefetching.pdf) |

### C. 微架构预取与硬件协同

| # | 论文 | 年份·会议 | 一句话 | 总结 | 翻译 | PDF |
|---|---|---|---|---|---|---|
| 13 | **Athena** Prefetch + OCP 协调 | arXiv '26（HPCA 待核） | 3 KB 表格 SARSA 协调预取与 off-chip 预测，奖励扣除"不相关项" | [总结](Bera-2026-HPCA-Athena-RL-Coordinating-Prefetch-and-OffChip-Prediction-总结.md) | — | [PDF](../papers/Bera-2026-HPCA-Athena-RL-Coordinating-Prefetch-and-OffChip-Prediction.pdf) |
| 14 | **PF-LLM** LLM-Hinted Prefetching | ASPLOS '26 | LLM 读 257 行汇编给每条 load 生成预取 hint，+9.8% IPC | [总结](Xu-2026-ASPLOS-PF-LLM-LLM-Hinted-Hardware-Prefetching-总结.md) | — | [PDF](../papers/Xu-2026-ASPLOS-PF-LLM-LLM-Hinted-Hardware-Prefetching.pdf) |
| 15 | **Stratum** Tiered Mono3D DRAM for MoE | MICRO '25 | 把 3D 堆叠的 WL 阶梯延迟变成 8 级 tier，解码吞吐 8.29× | [总结](Pan-2025-MICRO-Stratum-Tiered-Mono3D-DRAM-MoE-Serving-总结.md) | — | [PDF](../papers/Pan-2025-MICRO-Stratum-Tiered-Mono3D-DRAM-MoE-Serving.pdf) |

### D. GPU 显存与分类负载

| # | 论文 | 年份·会议 | 一句话 | 总结 | 翻译 | PDF |
|---|---|---|---|---|---|---|
| 16 | **DyNN-Offload** Learning-based Memory Mgmt | HPCA '24 | 用 6 种 memory idiom 编码架构，pilot model 预测张量需求，单卡可训 8× 大模型 | [总结](Ren-2024-HPCA-DyNN-Offload-Learning-based-Memory-Management-总结.md) | — | [PDF](../papers/Ren-2024-HPCA-DyNN-Offload-Learning-based-Memory-Management.pdf) |
| 17 | **RecMG** ML-Guided DLRM on Tiered Memory | arXiv '25（HPCA 待核） | 双向 Chamfer 距离当损失，on-demand fetch 少 2.2–2.8× | [总结](Ren-2025-HPCA-RecMG-ML-Guided-Memory-Optimization-DLRM-Tiered-Memory-总结.md) | — | [PDF](../papers/Ren-2025-HPCA-RecMG-ML-Guided-Memory-Optimization-DLRM-Tiered-Memory.pdf) |

### E. 综述

| # | 论文 | 年份·会议 | 一句话 | 总结 | 翻译 | PDF |
|---|---|---|---|---|---|---|
| 18 | **Savva 等** ML-Based Caching and Tiering — A Survey | ACM CSUR 58(11) '26 | 系统梳理 ML 在缓存淘汰/准入/预取与分层放置/迁移的用法与证据强度 | [总结](Savva-2026-ACMCSUR-ML缓存与分层综述-翻译总结.md) | [翻译](Savva-2026-ACMCSUR-ML缓存与分层综述-完整翻译.md) | [PDF](../papers/Savva-2026-ACMCSUR-Machine%20Learning-Based%20Caching%20and%20Tiering%20in%20Modern%20Data%20Storage%20Systems%20-%20A%20Survey.pdf) |

**厂商文档（非论文，不单独做总结）**：AMD MEXT 三份白皮书 + 安装指南 + 产品简介，分析与数据已并入根目录 [`AMD-MEXT-技术洞察报告.md`](../AMD-MEXT-技术洞察报告.md)。

---

## 二、命名与元信息待核实

| 文件 | 问题 | 依据 |
|---|---|---|
| `Bera-2026-HPCA-Athena` | PDF 首页**无会议名/DOI**，仅 arXiv:2601.17615；致谢提到 "MICRO 2025 and HPCA 2026 reviewers" | 会议归属待查 HPCA'26 官方程序 |
| `Ren-2025-HPCA-RecMG` | PDF **无会议名/DOI**，仅 arXiv:2511.08568（2025-11-11）；HPCA'25 已于 2025-03 召开 | 应记为 arXiv 预印本，除非另有录用证明 |
| `Ren-2024-HPCA-DyNN-Offload` | PDF 印 HPCA 页眉但**未印年份/DOI**；参考文献最新为 2023 | 年份来自文件名 |
| `Oh-2025-IEEEMicro-ExPAND` | 自述"IEEE Micro 录用论文的扩展版"，**未印卷期/页码** | 期刊卷期待补 |

（此前已修正一处：TPP 原文件名标 EuroSys，实为 **ASPLOS '23**。）

---

## 三、按问题聚类

### ① 页级分层：谁是决策者

| 论文 | 决策依据 | 执行机制 | 主指标 |
|---|---|---|---|
| TMO | PSI（应用侧损失） | 内核 reclaim + zswap/SSD 后端 | 内存节省 % |
| TPP | LRU + hint fault + 一拍迟滞 | 异步 migrate 到 CXL 节点 | 吞吐（归一 All-Local） |
| Equilibria | 每容器份额（protection/bound） | cgroup + demote/promote 调节 | 各租户 SLO |
| xTier | 学习排序（32 特征 MLP，内核内） | eBPF 决策 + 内核模块批量 migrate | 加速比 + 迁移量 |
| IDT | PPO 选各层降级阈值（四层真实硬件） | DAMON 区域监控 + migrate_pages | 执行时间倒数 |
| MANTA | 回归未来折扣访问次数（离线 + 轻量在线） | 秩过滤 + 迁移（基于 ARMS） | 加速比 vs ARMS |

### ② 改变输入而非策略

| 论文 | 做法 | 为什么不改后端 |
|---|---|---|
| OBASE | 重组虚拟地址空间，使页内冷热一致 | kswapd/TMO/TPP/Memtis 零修改即受益 |

### ③ 先算再决定（模型/离线）

| 论文 | 产出 | 前提 |
|---|---|---|
| Camp | 三分量减速解析模型 + 闭式最优交织比 | 设备带宽未饱和；11–12 个 PMU 计数器 + 每平台标定 |
| MANTA | 离线最优 ChOMP（可解 630× 更长 trace） | 有 trace、可离线训练 |
| PF-LLM | 每条 load 的预取 hint 表（7 B/PC） | 静态二进制、无 JIT、暂不支持 ASLR |

### ④ 策略该怎么拆（协调问题）

| 论文 | 拆法 | 反例证据 |
|---|---|---|
| Harmonia | 放置 agent + 迁移 agent，奖励耦合 | 单智能体（SAPM）比最佳基线差 21.9%；四种"组合技术"不如其成分 |
| Athena | 一个 agent 协调两个**机制**（预取器 + OCP） | Naive 组合反而在 40 个 adverse 负载上退化 11.2% |
| Baleen | 接纳 + 预取两段式 | 误判代价不对称（FP 付全量写成本） |
| ReStore | 每**层**一个 agent、共享奖励 | 单 agent 难以兼顾层间差异 |

### ⑤ 存储/flash 侧（慢层是 SSD）

| 论文 | 关键洞察 |
|---|---|
| Baleen | 瓶颈是**峰值** disk-head time 与写寿命，不是命中率 |
| ReStore | 设备属性（读写不对称、并发度）必须进状态；每层一 agent 只花 4 个参数 |
| ExPAND | 预取的胜负在**准时**（拓扑感知的端到端延迟），不在准确率 |
| Harmonia | 迁移可藏进空闲期；写放大略升换延迟大降 |

### ⑥ GPU / 硬件侧

| 论文 | 层次 | 关键洞察 |
|---|---|---|
| Stratum | DRAM 内部（3D 堆叠层） | 工艺缺陷（WL 阶梯延迟）可反用为 8 级 tier |
| DyNN-Offload | GPU 显存 ↔ CPU 内存 | 预测"代码要进哪个 block"比预测"数据何时被访问"容易 |
| RecMG | GPU 显存 ↔ CPU 内存（embedding） | 标签空间爆炸时，改用集合相似度（Chamfer）做损失 |

---

## 四、引用关系（已逐篇核对参考文献）

```mermaid
graph LR
  TMO["TMO<br/>ASPLOS'22"] -->|"TPP 引用并对比"| TPP["TPP<br/>ASPLOS'23"]
  TPP -->|"唯一基线 +52%"| EQ["Equilibria"]
  TPP -->|"tiering 后端"| OBASE["OBASE"]
  TPP -->|"对比策略"| XT["xTier"]
  TPP -->|"基线 1.48x"| IDT["IDT"]
  TPP -->|"基线之一"| MANTA["MANTA"]
  TMO -->|"叠加再省 15-30%"| OBASE
  TMO -->|"正交可叠加"| TPP
  MEL["Melody"] --> CAMP["Camp"]
  SOAR["SoarAlto"] --> CAMP
  SIBYL["Sibyl<br/>ISCA'22"] --> RE["ReStore"]
  SIBYL --> HAR["Harmonia"]
  SIBYL --> IDT
  ATMEM["ArtMem"] --> MANTA
  HAR -.->|"被引 ref[135]<br/>声明正交"| ATH["Athena"]
  ARMS["ARMS"] --> MANTA
  DYN["DyNN-Offload"] -.->|"同组前作"| REC["RecMG"]
  subgraph 无引用关系（仅主题相近）
    BAL["Baleen"]
    EXP["ExPAND"]
    STR["Stratum"]
    PFL["PF-LLM"]
  end
```

**要点**：

- **TPP 是公共坐标系**：OBASE、xTier、Equilibria、IDT、MANTA 全部以它为基线或对比对象，因为它进了主线内核——"和 TPP 比"是可复现的承诺。
- **Sibyl 是 RL 分层的起点**：ReStore 直接以它为基线（单请求 0.8 µs vs 27.4 µs），Harmonia 在它基础上加迁移 agent，IDT 也把它列入相关工作。
- **TMO 与 TPP 是串级而非竞争**：TPP 把 TMO 的换出路径改成"先 demote 再 swap"，stall 70%→40%、节省 13.5%→16.5%。
- **Camp 的基线集合与其余全部不重叠**（NBT/Colloid/Alto/Soar/Caption）。
- **Harmonia 是唯一被"微架构侧"论文引用的**（Athena ref[135]），但被明确声明为正交。
- **Baleen / ExPAND / Stratum / PF-LLM 与本组其余论文无引用关系**，只在本索引里按主题并列。

---

## 五、指标口径对照（跨论文比较前必读）

| 论文 | 主指标 | 分母/基线 | 误差报告 |
|---|---|---|---|
| TMO | 内存节省 %（固定尾部延迟目标下） | 部署前自身 | 无（生产 A/B） |
| TPP | 吞吐，归一化 | Ideal All-Local | 3 次平均 |
| Equilibria | 各租户 SLO 达成 + 相对吞吐 | Linux/TPP | 未报 CI |
| Camp | **预测误差**（%/相关性） | 实测减速 | 按负载分布 |
| OBASE | 页利用率 + RSS | 无 OBASE 基线 | 未报 CI |
| xTier | 归一化加速比 + 迁移量 | 最慢 system-ratio 配置 | 未报 CI |
| IDT | 执行时间倒数 | 默认 Linux | 未报 CI |
| MANTA | 加速比 | ARMS（同内核版本） | 3 次重复 |
| Baleen | Peak DT / TCO | 固定写速率下的最佳基线 | 仿真 + 点验证 |
| ReStore | 运行时 / 迁移次数 | 8 个策略 + Oracle | 未报 CI |
| Harmonia | 平均 I/O 延迟 | Fast-Only | 3 次平均 |
| ExPAND | 加速比 | NoPrefetch / 其他预取器 | 仿真是单一配置 |
| Athena | IPC（geomean） | Naive / HPAC / MAB / StaticBest | 仿真，单一配置 |
| PF-LLM | IPC | 最佳单预取器 / 最佳 ensemble | 仿真 |
| Stratum | 解码吞吐 / 能效 | GPU 基线 | 仿真 |
| DyNN | batch size / 训练时间 | UVM / DTR / ZeRO | 未报 CI |
| RecMG | on-demand fetch / 推理时间 | 6 缓存 + 7 预取基线 | 仿真 + 分析模型 |
| MEXT 白皮书（厂商） | 保留率 = 保留性能 ÷ 保留 DRAM 比例 | all-DRAM 同容量 | 无标准差 |

**三个坑**：

1. 只有 MEXT 白皮书把结果折算成"每 DRAM 美元"，其余全是无量纲比值；
2. **Camp 报的是预测误差、不是系统性能**，与其余不同层，不能混入性能表；
3. 各论文对"基线"的定义不同（同内核 vs 上游 vs 理想），跨论文引用数字前先对齐分母与延迟比。

---

## 六、尚未覆盖的部分

| 项目 | 状态 |
|---|---|
| TMO / TPP / Equilibria / Camp / xTier / IDT / MANTA / Baleen / ReStore / ExPAND / Athena / PF-LLM / Stratum / DyNN / RecMG | 只有总结，无全文翻译 |
| AMD MEXT 白皮书 ×3 + 安装指南 + 产品简介 | 无独立总结，已并入 `AMD-MEXT-技术洞察报告.md` |
| `xtier/xTier-2026-...pdf` | 与 `papers/` 中同名文件字节相同，属重复副本 |

需要哪篇补全文翻译，直接点名即可。
