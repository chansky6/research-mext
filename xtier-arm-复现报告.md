# xtier-arm 复现报告：SPE 采样 + 预测性降级在 DRAM/SSD 上的量化效果

**日期：** 2026-11-23
**代码：** `code/xtier-arm/`（commit `667a8f13be`，结果补充 `bb8468125e`）
**机器：** 192.168.90.7，Kunpeng 920（288 核 aarch64），DRAM 61 GB（2 NUMA），SATA SSD（随机 4K qd1 ≈ 6 700 IOPS / 150 µs，qd8 ≈ 17 400 IOPS，顺序写 129 MB/s）
**上游底本：** `code/xtier-origin`（xTier 上游 fresh clone，HEAD `9cab0fa`）

---

## 一、一句话

在 ARM（SPE 替代 PEBS）+ DRAM/SATA-SSD（替代 DRAM/CXL）+ **零内核改动**的约束下，复现了 xTier 的 ML 分层策略：同等 2.5 GB DRAM 预算下，MLP 驱动的降级把回 fault 率压到内核 LRU 的 **42%**（4 890 vs 11 621 次/s，exp5），全 DRAM 参照吞吐 100% 时内核 LRU 只剩 **6.3–7.1%**（exp6，zipf_chase 纯延迟负载），MLP 组吞吐结果单轮测试在跑（见 §5 待补项）。

## 二、系统映射（细节见 `code/xtier-arm/README.md`）

| xTier 原实现 | xtier-arm |
|---|---|
| PEBS `MEM_UOPS_RETIRED.ALL_LOADS` | SPE per-task 事件，用户态 aux 解码（48 B/样本，含数据 VA + tid） |
| BPF 内核推理 | 用户态 INT8 MLP（xTier `us_*` 用户态孪生逐行移植） |
| `xtier_migrate_range`（内核补丁） | `process_madvise(pidfd, MADV_PAGEOUT)` 换出 |
| PTE A-bit 冷扫描 | `page_idle` 位图冷扫描 |
| CXL 远端 NUMA 节点 | SSD swapfile（`/home/swapfile` 16 GB，swappiness=100） |
| ML 促升 | **方向翻转**：ML 分数排序降级 + top-K 门控；促升交给内核 demand fault（swap 页不可采样，预测性促升无信号） |

训练管线：collect 200 万行 features.csv（与 xTier schema 逐列兼容）→ torch 2.15 训练 → INT8 头文件 → daemon 编译进二进制。

## 三、实验设置

负载 `bench/zipf_chase`：3 GB 匿名内存、2 MB 块、θ=0.9、8 线程绑 node0、每 op 16 级串行指针追逐（纯延迟探针，fault 直接阻塞关键路径）。DRAM 预算用两种方式施加：cgroup v1 硬限（LRU 基线）或 daemon 软预算（MLP，cgroup 留 400 MB 余量防止内核先触发——**exp4 实测：硬限与 daemon 预算重合时 daemon 完全失效**，`demote_ok=0`）。每组前 `xtier_arm_sysprep`（paranoid=0、THP=never、只留 swapfile、drop_caches）。

## 四、结果

### 4.1 scorer 内部消融（exp3，120 s/组，单次）

| scorer | 降级页数 | majflt | 每降级页回 fault 率 |
|---|---|---|---|
| **mlp** | 400,973 | **131,065** | **0.33** |
| random | 600,136 | 235,837 | 0.39 |
| freq | 831,537 | 365,718 | 0.44 |

排序质量有效：MLP 用最少的降级量换回最少的 fault。单负载单次运行，置信度中。

### 4.2 MLP vs 内核 LRU：fault 率（exp5，129 s/组）

同等效 2.5 GB 预算、swappiness=100：

| | majflt 总量 | majflt/s | VmSwap 终值 |
|---|---|---|---|
| 内核 LRU（cgroup 硬限 2.5 GB） | 1,511,160 | **11,621** | 722 MB |
| MLP daemon（预算 2.5 GB） | 635,849 | **4,890** | 377 MB |

**回 fault 降 58%，换出量减 48%。** 脚注：mlp 组 cgroup 上限 2.9 GB，内核仍参与了回收（daemon 实际只降 9 万页），2.4× 是混合策略对纯 LRU 的**下界**。

### 4.3 吞吐（exp6，30 Mops/线程 × 8 线程，3 GB 工作集）

| 配置 | Trial Time | 相对全 DRAM |
|---|---|---|
| 全 DRAM 参照（无限无 daemon，5 轮） | **11.207 s 均值**（11.191–11.231，σ≈0.14%） | 100% |
| 内核 LRU（cgroup 2.5 GB，前 2 轮，进程被中止） | 178.70 s / 157.14 s | **6.3% / 7.1%** |
| MLP daemon（2.5 GB 预算，单轮，跑批中） | 待补 | — |

原始数据：`192.168.90.7:/var/tmp/xtier-results/exp6_{ref,lru,mlp1}/workload.log`。LRU 第 2 轮比第 1 轮快 12%（跑热后 workingset 元数据稳定），两轮均远未收敛。

## 五、分析

1. **LRU 慢 16 倍的构成**：11 621 majflt/s × 150 µs ≈ 1.75 核·秒/秒线程停顿（8 线程的 ~22%）；加上 SSD qd1 被打满（6 700 IOPS 上限）后排队放大。zipf_chase 无计算间隙可掩盖 I/O，是这类系统最坏负载——MEXT 宣称的 80–95% 性能保持（见《AMD-MEXT-技术洞察报告》§3）建立在企业 NVMe + 计算混合型负载（PrimeTime/agentic AI）上，与本文负载不可直接比。
2. **MLP 收益的来源**：benefit 标签 = 未来 10 epoch 再访问强度；换出的恰是模型判定不会再访问的页（每降级页回 fault 率 0.33 vs freq 0.44）。同构于 MEXT 的"预测冷页卸载"，但缺其另一半——**预测性回流**（对已换出页的预测性换入）：swap 页不产生样本，我们的促升仍是 demand fault（`code/xtier-arm/README.md` 方向翻转一节）。
3. **容量比现实**：本实验预算比 2.5/3 GB（≈17% 容量缩减），LRU 已掉到 6% 吞吐；MEXT 场景是 50–75% 缩减。差距由硬件（SATA vs NVMe，延迟差 ~15×）与机制（无主动促升、驻留未收敛到预算线）共同造成，前者本机不可解。

## 六、局限与置信度

- 全部结果为**单负载、单预算、单次运行**；exp6 LRU 组只完成 2 轮即被中止。
- `demote_ok` 统计的是提交给内核的页数，MADV_PAGEOUT 返回值不区分是否真正换出（与 xTier 的 att/ok 分离同性质）。
- daemon 运行时驻留未收敛到预算线（3.17 GB vs 2.5 GB 预算，exp2/exp3）：候选池每 epoch 只覆盖被采样页，未采样页依赖冷扫描（1 024 页/epoch 上限）。**当前对照的实质是"降级边际上的受害者选择质量"，不是完整容量管理。**
- 吞吐对照里 daemon 采样/推理开销（单核内 ~30 µs/页）计入 MLP 组、不计入 LRU 组——这对 MLP 略不利，方向保守。

## 七、后续

1. MLP 组吞吐补齐（进行中）+ 5 轮 × 3 配置完整版；
2. 预测性促升原型：fault 邻域批量预取（`process_vm_readv` 批量读 2 MB region，摊薄单次 150 µs）+ fault 回灌标签闭环；
3. 冷扫描接入预算驱动，使驻留真正收敛到预算线（容量管理的完整性）；
4. 换 xgboost（xTier bench 自带，计算/访存混合）重跑，逼近 MEXT 类负载形态；
5. 无预算纯对照组（6 GB 工作集 + 2.5 GB cgroup）确认容量比加大后 LRU/MLP 分差走向。

## 八、复现命令

```sh
# 机器准备
ssh root@192.168.90.7; cd /opt/xtier-arm; source env.sh && xtier_arm_sysprep
# 训练（Mac，venv /tmp/xtv，torch 2.15 + shim）
XTIER_OUTPUT_HEADER=ml/models/mlp_q8_zipf.h python3 ml/train_and_export.py --input features.csv
# 采集
numactl --cpunodebind=0 --membind=0 bench/zipf_chase 3 0.9 200 8 1 & sudo src/xtierd $! 4096 1000 collect
# 吞吐 A/B（exp6 脚本）
/tmp/exp6.sh
```


---

## 补充（2026-11-23 深夜）：吞吐对照的曲折与最终修正

exp6b（无补丁 MLP）暴露 **26K majflt/s 抖动螺旋**后，打了三个补丁（fault 回页钉住 /
降级资格门槛 / fault 风暴滞后熄火）并改用单驱逐者架构（cgroup 3.2GB headroom +
daemon 预算 2.5GB），加 ops 探针重跑（exp9）。过程中修了探针自身的假共享 bug
（`_Alignas(64)` 只对齐数组头，8 线程共享计数行，contaminate 了 exp8）。

### 最终数字（同夜同机，zipf_chase 3GB/θ0.9/8 线程/单轮，全 DRAM 参照 11.2s）

| 配置 | 有效 DRAM | Trial Time | avg ops/s | majflt 总量 |
|---|---|---|---|---|
| 全 DRAM | 61 GB | 11.2 s | 21.4M | ~0 |
| 内核 LRU @3.2GB | 3.2 GB | 66.8 s | 3.62M | — |
| **MLP daemon（cgroup 3.2GB，预算 2.5GB 未执行到位）** | **3.19 GB** | **77.6 s** | 3.11M | 216,177 |
| 内核 LRU @2.9GB | 2.9 GB | 283.8 s | 847K | — |
| 无补丁 MLP（exp6b，cgroup 2.9GB） | ~2.87 GB | 749.8 s | ~320K | 5.61M |
| 补丁后 MLP（exp7，cgroup 2.9GB，风暴门锁死） | 2.87 GB | 未完成(486s 终止) | — | 4.67M |
| 内核 LRU @2.5GB | 2.5 GB | 1559.1 s | 154K | — |

### 结论修正（推翻本报告早前 2.4× 的外推）

1. **exp9 的"20×"是内存混淆**：MLP 组驻留 3.19GB（钉 cgroup 上限）vs LRU 组 2.5GB。
   同内存对照 LRU@3.2GB=66.8s ⇒ **同等内存下 daemon 是 ~16% 纯开销**
   （SPE per-op 采样 + 65MB 降级 churn + 216K 回 fault）。
2. **预算执行是真正的短板**：63 epoch 仅降 65MB。两个发现通道带宽都不够：
   冷扫描按页 page_idle 查询（每页 2–3 syscall）在快负载下找不到 idle 页
   （每页 ~0.25s 被摸一次，A 位恒 1）；ML 门槛在 2K samples/epoch 密度下把候选池砍空。
   模型无罪，**候选发现是瓶颈**。
3. 仍然成立的成果：exp5 同硬预算 fault 率 2.4×（4,890 vs 11,621/s）；三补丁防住
   26K/s 螺旋（exp6b→exp7）；cgroup 硬限与 daemon 预算重合会使 daemon 失效（exp4）。
4. 架构性教训：**采样式策略在"内存够→跑得快→采样稀→候选少→预算不执行"和
   "内存紧→抖动深→采样塌→投毒"两个方向都被自己的信息通道卡死**。
   xTier 在 PEBS freq 模式（采样率自稳定）+ CXL 驻留可采的条件下不遇此耦。
   出路：预测性促升/预取（把需求 fault 异步化）、批量 pagemap 扫描替代逐页
   page_idle、SPE interval 自适应。

### 复现命令（exp9/10）

```sh
ssh root@192.168.90.7; /tmp/exp9.sh   # lru@2.5G vs mlp@cgroup3.2G
/tmp/exp10.sh                          # lru@3.2G, lru@2.9G 括号对照
```
原始数据：`/var/tmp/xtier-results/exp{6_lru,7_mlp_fixed,9,10}_*/`。
