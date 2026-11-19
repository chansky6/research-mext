# TMO 论文总结

**TMO: Transparent Memory Offloading in Datacenters**
Johannes Weiner, Niket Agarwal, Dan Schatzberg, Leon Yang, Hao Wang, Blaise Sanouillet, Bikash Sharma, Tejun Heo, Mayank Jain, Chunqiang Tang（Meta Inc.）、Dimitrios Skarlatos（CMU）
ASPLOS '22，pp. 609–621，DOI 10.1145/3503222.3507731（Senpai 开源：`github.com/facebookincubator/oomd`；PSI 与 reclaim 改动已进上游）

> 与内存分层的关系：这是**页级分层**（page-level tiering）一类工作的基准基线，OBASE 把它列为对比后端之一，TPP 把它作为主要比较对象并证明可叠加。

## 一句话

用 **PSI（Pressure Stall Information）** 把"内存压力"从代理指标换成**直接度量损失的工作量**，让一个用户态控制器每 6 秒决定"卸载多少"，内核决定"卸载哪些"——在生产数百万台服务器上跑了 **>1 年，节省全 fleet 总内存的 20–32%**。

## 问题

1. DRAM 将占到服务器成本的 **33%**、功耗的 **38%**，而压缩内存还不够便宜，需要 NVMe SSD 这类更低成本的层级（系统级成本 <3%，同容量下 <1%，约为压缩内存的 1/10 每字节）。
2. 当时唯一大规模部署的内核换页方案 **g-swap（Google 的 zswap）**只支持**单一慢层**（压缩内存），且对难压缩数据（ML 量化模型仅 1.3–1.4×）无效。
3. g-swap 依赖**离线 profiling + 静态 promotion rate** 目标。这个指标不反映应用对变慢的敏感度，也不考虑卸载设备的性能——Meta 实测出现反例：在更快的设备上，更高的 promotion rate 反而性能更好。
4. 内核 reclaim 历史上**严重偏向 file cache**，swap 只当"紧急溢出"用；且需同时覆盖匿名内存、file cache、应用容器与 sidecar 容器。

## 机制

**① PSI：度量"因为缺资源而没做出来的工作量"**

按非空闲进程统计 runnable 时间与 stalled 时间，定义 compute potential = min(非空闲进程数, CPU 数)；PSI = 因资源 stall 而未能产出的 compute potential 比例。每个 cgroup/整机给出 `some`（至少一个进程被 stall 的时间占比）与 `full`（全部进程同时被 stall）。内存 PSI 只统计三类事件：分配时触发 reclaim、最近被逐出 file page 的 refault IO 等待、swap-in 读阻塞。块 IO 一律记为"IO 不足"（论文承认这是近似）。

**② Senpai：以 PSI 为反馈的回收量控制**

每隔几秒对每个 cgroup 计算：

$$\text{reclaim\_mem} = \text{current\_mem} \times \text{reclaim\_ratio} \times \max\left(0,\; 1 - \frac{\text{PSI}_{some}}{\text{PSI}_{threshold}}\right)$$

PSI 高于阈值则**不回收**，逼近阈值则逐步减量——目标是维持"低但非零"的轻微压力。生产参数：`reclaim_ratio = 0.0005`、`PSI_threshold = 0.1%`、每 **6 秒**一次、单次最多回收总量的 **1%**。早期版本靠调 cgroup `memory_limit`，因有状态会阻塞增长中的容器，为此新增**无状态**的 `memory.reclaim` 接口。收缩反应时间是**分钟级**，扩张可即时响应。

**③ 内核 reclaim 改造：refault 感知的 file/anon 平衡**

引入 per-cgroup 非驻留缓存计数与 shadow entry，fault 时用 reuse distance 判定 refault。规则：**没有 refault 就只回收 file cache；一旦出现 refault，就按 refault rate 与 swap-in rate 平衡 file 与 swap 的回收比例**。这让 swap 在 file cache 工作集刚开始被回收时就启动，从"紧急溢出"变成"阈下压力的常规换页"。

**④ 后端可插拔**：zswap（DRAM 内压缩池，fault 时解压无需磁盘 IO）或 NVMe SSD swap；file cache 直接丢弃。同一套代码可跑两者，因为回收速率由 PSI 自动适配后端速度。生产选型：压缩算法 **zstd**、内存池分配器 **Zsmalloc**。对 SSD 写入限速，全 fleet 分析得出 **1 MB/s** 为安全阈值。

**⑤ 覆盖范围**：应用容器 + datacenter tax（日志、服务发现）+ microservice tax（路由、代理 sidecar）。

## 评估设置

生产 Skylake **64 GB** 主机，常以"数十台主机"做 A/B（生产 load-testing 框架：固定尾部延迟目标下的 RPS，服务器自动限流以守住尾部延迟）。后端：zswap 与 SSD swap（并对比快 SSD = C 与慢 SSD = B）。基线：无 offload 生产基线（swap 关闭）、**g-swap**。负载：8 类应用（Ads A/B/C、Web、Warehouse、Feed、RE、ML Reader、Cache A/B、Analytics、Video），重点剖析 Meta 最大应用之一 **Web**。

## 关键数字

| 指标 | 数值 |
|---|---|
| 全 fleet 总内存节省 | **20–32%**（数百万台服务器，>1 年生产） |
| 应用容器内存节省 | **7–19%** |
| memory tax 节省 | 约 **13%**（datacenter tax 9% + microservice tax 4%） |
| 压缩内存后端节省 | **7–12%** 常驻内存（5 个应用） |
| SSD 后端节省 | **10–19%** 常驻内存（g-swap 不可达） |
| Web 应用 | 消除 20% RPS 下滑 ⇒ **20% 容量节省** |
| 压缩内存 4 KB 读延迟 | p90 约 **40 µs**（SSD p99 读写 470 µs–9.3 ms） |
| Senpai 回收的 CPU 开销 | 全部 CPU 周期的 **0.05%** |
| 冷内存机会 | 平均 **35%**（范围 19–62%） |
| 快 vs 慢 SSD | 快 SSD 多卸载 1.6–3 GB（≈10–15% RSS） |
| 平均内存税占比 | 服务器总内存 **20%**（datacenter 13% + microservice 7%） |

**注意口径**：论文**没有** refault-rate 的数值目标（唯一的控制目标是 `PSI_some ≤ 0.1%`）；**没有**报告 CPU 节省百分比（只有 0.05% 的开销）；**没有**给出尾部延迟的毫秒数或改善比例——其性能口径是"在固定尾部延迟目标下，RPS 不再下滑 ⇒ 容量省 20%"。

## 论文自陈的局限

1. **卸载后端靠人工选**：按应用可压缩性与访存敏感度在 zswap 与 SSD swap 之间手动二选一；期望未来由内核管理"卸载后端层次结构"（暖页用 zswap、冷/难压缩页用 SSD，并纳入 NVM/CXL），但明确写着"正在做"，尚未实现。
2. **参数靠经验**：6 秒周期、0.1% 阈值等"set this value empirically"，自动/在线调参留作未来工作；批处理类负载（SLO 宽松）本可承受更高压力、获得更多节省，未利用。
3. **收缩反应慢**：极端收缩的反应时间是分钟级。
4. **PSI 归因是近似**：无法区分块 IO 等待是设备过载还是正常延迟，因此一律记为 IO 不足。
5. **需要硬件辅助**：zswap 受益于硬件压缩/解压；当前靠软件采样维护 LRU，开销随目标换页率线性增长。

## 我的判断

**TMO 的贡献是把控制变量换掉了。** 从"promotion rate 越低越好"换成"应用损失了多少工作量"——前者是系统侧代理，后者是应用侧真值。g-swap 的反例（更快的设备上更高 promotion rate 反而更好）是这条论证的支点：任何与设备性能无关的固定目标，必然在某一类设备上出错。

**它是"反馈控制"范式的完整形态**：目标可观测（PSI）、执行器可调节（memory.reclaim）、闭环有阻尼（逼近阈值时减量）。相比之下，我们讨论的 xTier 是"预测 + 准入"的前馈形态，OBASE 是"改变布局让下游更易判断"的结构形态。三者不是替代关系——TPP 论文已实证 TMO + TPP 叠加后 stall 从 70% 降到 40%、节省从 13.5% 升到 16.5%；OBASE 论文也报告叠加 TMO 可再省 15–30%。这说明**反馈控制缺的是"输入信号质量"**，而布局工程正好补这一环。

**最值得学的一条工程细节**：把 cgroup `memory_limit` 换成无状态的 `memory.reclaim`。有状态的限流会阻塞正在增长的容器——这是可控性设计问题，不是算法问题。
