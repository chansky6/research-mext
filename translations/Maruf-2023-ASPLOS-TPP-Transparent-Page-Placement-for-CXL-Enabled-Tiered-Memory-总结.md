# TPP 论文总结

**TPP: Transparent Page Placement for CXL-Enabled Tiered-Memory**
Hasan Al Maruf、Hao Wang、Abhishek Dhanotia、Johannes Weiner、Niket Agarwal、Pallab Bhattacharya、Chris Petersen、Mosharaf Chowdhury、Shobhit Kanaujia、Prakash Chauhan（Michigan / NVIDIA / Meta）
**ASPLOS '23**（Vol. 3, pp. 742–755，DOI 10.1145/3582016.3582063）；arXiv:2206.02878v2

> ⚠️ 本仓文件名曾标为 EuroSys，实为 ASPLOS '23；已按事实重命名 PDF。

## 一句话

把 CXL 内存当成"**可降级的 NUMA 节点**"而不是 swap：用默认 LRU 选冷页、**异步 migrate 到 CXL**（不换出），把"分配水线"与"降级水线"解耦、始终留出余量，再用 **NUMA hint fault + active-LRU 一拍迟滞**把热页拉回来——于是分层系统达到"全部内存都在本地"理想基线的 **<1%** 差距，比当时的 Linux 好 **18%**，比 NUMA Balancing / AutoTiering 好 **5–17%**。

## 问题

1. 内存已占机架 TCO/功耗的 15.6%–37.1%，而 Linux 内存管理是为**同构 DRAM** 设计的：本地节点被填满后，新分配被挤到 CXL 节点并长期滞留。
2. CXL 设备的延迟 170–250 ns（无 CPU 的 NUMA 节点），swap 抽象**丢失了 load/store 语义**——这是关键洞察：既然 CXL 可直接寻址，就该"迁移页"而不是"换出页"。
3. 缺少生产可用的页热度刻画：IPT 类工具要改内核、开销 20–90%；PEBS 用户态工具每核 >15% CPU，且无法按页类型出热图。
4. 现有方案各自失效：NUMA Balancing 在本地缺内存时**停止 promotion**且无迟滞；AutoTiering 预留固定缓冲，压力下同样失效（1:4 配置 warm-up 后直接崩）。

## 机制

**① Chameleon 刻画工具（p3–5）**：PEBS 采 `MEM_LOAD_RETIRED.L3_MISS`，**每 200 次事件采 1 次**，核分组轮转（mini_interval 5 s、interval 1 min，双哈希表切换）；每页 64 位 bitmap 记录区间活跃位。开销：生产单核 3–5%；带宽敏感合成负载全核时损失 7%。

**② 轻量降级（p7）**：复用默认 LRU 选候选，但**同时扫 inactive anon 与 inactive file**；候选进单独 demotion list，**异步 migrate 到 CXL 节点**（比 swap 快数个数量级）。迁移失败则回退默认回收。

**③ 分配与回收解耦（p7–8）**：在原有 min/low/high 之外新增两个门限，且 **demotion > allocation > low**，从而始终保留 headroom。触发由 `/proc/sys/vm/demote_scale_factor` 控制，**默认 2%**（本地可用容量剩 2% 时启动回收；作者注明是经验值）。

**④ 热页提升（p8）**：复用 NUMA Balancing 的 hint fault（默认每节点采样 256 MB），但**只对 CXL 节点采样**。命中 **active LRU** → 晋升；命中 **inactive LRU** → 不迁移，只置 accessed 并移到 active，**下一次** hint fault 仍热才晋升——这就是 TPP 唯一的迟滞机制（一拍）。晋升故意**跳过目标节点水位检查**，以制造压力触发冷页回收。

**⑤ 页类型感知分配（可选，p8–9）**：file cache / tmpfs 初始分配到 CXL 节点，anon 保持本地优先；变热的 file 页仍可被拉升。

**⑥ 可观测性（p9）**：`/proc/vmstat` 增加按 anon/file、按内存类型、按失败原因分类的 demotion/promotion 计数器；用页标志 **0x40 = `PG_demoted`** 统计"降级后又成晋升候选"的抖动。

**内核基线**：基于 Linux v5.12，大部分补丁进入 **v5.18**（这是它成为后续论文默认基线的根本原因）。

## 评估设置

功能验证用预生产 x86 + **FPGA CXL 1.1 扩展卡**（延迟比目标硬件高约 250 ns）；性能评估用**双路服务器模拟**（一路只留内存节点当 CXL，另一路提供全部活跃核 + 单内存节点）。配置：local:CXL = **2:1**（生产形态）与 **1:4**（压力测试）。基线：Ideal All-Local、Default Linux、NUMA Balancing、AutoTiering、TMO。负载：Web1（HHVM）、Web2（Python）、Cache1/2（tmpfs 对象缓存）、Data Warehouse、Ads1–3。

## 关键数字

| 指标 | 数值 |
|---|---|
| 整体 | 与 Ideal All-Local 差距 **<1%**；比 Default Linux **+18%**；比 NUMA Balancing/AutoTiering **+5–17%** |
| Web1（2:1）吞吐 | **99.5%**（Default 83.5%、NUMA Bal 82.8%、AutoTiering 87.0%） |
| Cache1 / Cache2（1:4） | **99.5% / 95.0%**（Default 86.0% / 82.0%） |
| 本地节点内存流量（Web1, 2:1） | **90%**，Default 仅 **22%** |
| 回收速度 | 比 Default Linux **快 44×**；比 NUMA Balancing 回收快 42×、晋升快 11× |
| 冷内存占比 | 任意 2 分钟窗口内 **55–80%** 空闲；Data Warehouse 仅 20% 为热 |
| 降级/晋升速率 | 稳态迁移带宽 4–16 MB/s（1–4 K 页/s）；晋升平均 **50 KB/s**、p99 1.2 MB/s |
| CXL 容量利用（Web1/Cache1/DW, 2:1） | TPP **83%/92%/87%** vs TMO **45%/61%/7%** |
| 迟滞的收益 | 晋升速率 −11×、demote→promote −50%、晋升成功率 +48%、吞吐 +2.4%（收敛慢 5 分钟） |
| CXL 延迟 220–300 ns 变化 | TPP 仅 **4–5%** 热页落在 CXL（Default 22–25%），平均访存延迟差 **7×** |

## 论文自陈的局限

1. **无多租户公平性**：不同优先级的应用共置时可能次优，需在其上叠加 QoS 机制（这点直接催生了 Equilibria）。
2. **只做容量扩展，不做带宽扩展**：应把"带宽密集但延迟不敏感"的页也放到 CXL，留作未来工作。
3. **未用硬件加速**：memory-side cache/prefetch、硬件迁移支持都没用；本地内存极小时可能暴露迁移开销。
4. **硬件保真度**：FPGA 卡延迟比目标高约 250 ns，仅用于功能验证；性能数据来自双路 NUMA 模拟。
5. **参数经验化**：`demote_scale_factor = 2%` 是经验设定。
6. **评估覆盖**：受篇幅限制只选部分应用；AutoTiering 在 1:4 下崩溃故只跑 2:1。

## 我的判断

**TPP 是这一系列论文的"公共坐标系"**：OBASE、xTier、Equilibria 全部以它为基线，TMO 与它互为对照。原因不是它最聪明，而是它**进入了主线内核（v5.18）**——这让"和 TPP 比"变成了一个别人能复现的承诺。

它最硬的一个判断是**"swap 抽象错配"**：既然 CXL 可 load/store，就不该用换页机制管理它。这条判断的价值可以从两张表反证——TMO 只能填充 CXL 容量的 45%/61%/7%，而 TPP 达到 83%/92%/87%；同时 TPP 把 TMO 的换出路径变成"先 demote 再 swap"两段式后，进程 stall 从 70% 降到 40%、内存节省从 13.5% 升到 16.5%。**两个机制不是竞争，而是串成了两级。**

**它的弱点也由此而来**：`demote_scale_factor` 是全局经验值、晋升只对 CXL 节点采样、完全没有公平概念——这三点分别被后来的工作接走：水线/比例调参被 OBASE 的前端思想绕过（先改布局再分层）、多租户公平被 Equilibria 正面解决、决策质量被 learn 系工作（xTier/Memtis）改进。

**对复现的意义**：TPP 的机制足够简单（水线 + LRU + hint fault + 一拍迟滞），在没有 CXL 的环境里最容易近似；但它的收益**完全依赖"本地 vs CXL 的延迟差"**，实验里必须实测层间延迟比，否则 99.5% 这种数字没有意义。
