# OBASE：面向对象粒度的地址空间工程，改进内存分层（Memory Tiering）

**Vinay Banakar**（威斯康星大学麦迪逊分校 / Google）、**Suli Yang**（Google）、**Kan Wu**（xAI，工作在 Google 期间完成）、**Andrea C. Arpaci-Dusseau**、**Remzi H. Arpaci-Dusseau**（威斯康星大学麦迪逊分校）、**Kimberly Keeton**（Google）

第 20 届 USENIX 操作系统设计与实现研讨会（OSDI '26），2026 年 7 月 13–15 日，美国西雅图
ISBN 978-1-939133-55-7
<https://www.usenix.org/conference/osdi26/presentation/banakar>
开源代码：<https://github.com/WiscADSL/obase>

---

## 摘要

面向内存分层的硬件与操作系统机制已被广泛部署，但数据中心仍然对 DRAM 过度配置。根因是**热度碎片化（hotness fragmentation）**：分配器按**大小**而非访问模式放置对象，导致冷热对象在同一页面内交织。单个热对象会把整页标记为活跃，把周围的冷数据一起困在昂贵的 DRAM 中。我们对 Google 数据中心工作负载的分析显示，活跃页中最多 **97% 的字节是冷的、无法回收的**。

我们提出**地址空间工程（address-space engineering）**：动态重组虚拟内存，使热对象聚集成“纯热页”，冷对象聚集成“纯冷页”。我们实现了 **OBASE**——一个面向非托管语言的编译器—运行时系统，它作为**对象感知的前端（frontend）**服务于**页面感知的操作系统后端（backend）**。OBASE 通过轻量的指针插桩跟踪访问，并使用无锁协议在运行时迁移对象，且在并发下是安全的。通过重组地址空间，OBASE 使**未修改的**后端（kswapd、TMO、TPP、Memtis）能够有效地进行内存分层。在十种并发数据结构、六种后端以及来自 Meta 与 Twitter 的生产 trace 上，OBASE 将页利用率提升 **2–4 倍**、内存占用最多降低 **70%**，而额外开销仅 **2–5%**。

---

## 1 引言

内存容量——尤其是 DRAM——已成为现代数据中心的主导成本项，常占服务器资本开支的 50% [1, 49, 59]。内存分层提供了解决方案：把冷的、访问较少的数据放到更便宜、更慢的层级——压缩内存、SSD 或 CXL 挂载的解耦内存 [3, 4, 6, 11, 42, 43]——即可显著降低所需 DRAM 容量，从而降低成本 [29, 44, 49, 56, 60, 62]。原则上这一思路前景很好，主要因为超大规模负载中只有一小部分内存是热的，绝大多数内存在很长时间内不被访问。如图 1 所示，在六种 Google 工作负载 [13] 中，trace 期间只有 **1.7%–21.3%** 的字节被访问；其中四种工作负载访问的字节不足 3%，这意味着我们可以把 80%–98% 的 DRAM 让给二级存储层，而几乎没有性能损失。

**图 1. Google 工作负载中的内存访问粒度鸿沟。** 深灰：实际被访问的内存**字节**占比；浅灰：实际被访问的内存**页**占比；红色：页利用率（= 访问字节 / 访问页，详见 §2.2）。

然而实践中可实现的收益常常远低于这一潜力。Google 报告仅将 20% 的不常访问数据卸载到压缩内存层 [43]，Meta 也只取得了 20–32% 的温和节省 [62]。差距的症结在于**应用组织数据的方式与操作系统管理内存的方式不匹配**：应用以**对象**粒度访问数据，对象大小不一；而操作系统以固定大小的**页**（4KB、2MB、1GB）粒度管理内存 [20]。现代分配器放置对象时完全不考虑这些对象未来会如何被访问，常常把频繁访问的对象与罕被触及的对象混放在同一页上。结果，少量热字节（例如单个对象）就会让整页在 OS 视角下变为活跃页，把整页困在昂贵 DRAM 中。图 1 清晰展示了这一现象：工作负载 Bravo 中虽只有 3.2% 的**字节**被访问，却有 91.8% 的**页**被访问，使这些页完全不适合回收。六种工作负载都呈现类似趋势。我们把这一现象称为**热度碎片化**，并认定它是超大规模场景下分层效率低下的根因。我们用**页利用率**指标来量化该碎片化程度。

为对抗热度碎片化，我们引入**地址空间工程**：**动态**重组地址空间中的对象，把访问强度相近的对象归并到一起。把热对象与冷对象分离后，地址空间工程有望弥合理想内存节省与可实现节省之间的差距。它产生的内存布局由均匀的冷页（或热页）构成，对分层机制要友好得多。

关键在于，通过聚焦地址空间的**布局**，我们把内存分层**解耦为两个正交关注点**，允许二者独立创新：

- **布局（前端）问题**：如何组织对象，以产生高质量的回收候选页；
- **回收（后端）问题**：跨层迁移的策略与机制。

这种分离使我们能够复用现有的基于页的分层基础设施——无论基于 swap [43, 62] 还是字节可寻址 [44, 49, 56]——并享受未来回收机制的改进。它也保证我们的布局优化技术对未来尚不存在的内存层（例如 CXL 3.0 fabric 或 NVM）依然适用。此外，它显著降低了超大规模厂商的采用门槛：无需修改既有分层后端，地址空间工程只是提供了更优的内存布局，使既有后端工作得更好。

本文提出 **OBASE**（**O**bject-**B**ased **A**ddress-**S**pace **E**ngineering），一个为 C/C++ 之类**非托管语言**设计地址空间的编译器—运行时系统。OBASE 充当内存组织的智能**前端**：管理基于指针的数据结构，用轻量插桩透明地剖析对象访问以确定访问强度，并采用新颖的无锁协议安全地迁移对象，把热对象和冷对象分别聚拢。由此，OBASE 为任意 OS **后端**准备好内存布局，使它们工作得更高效。

OBASE 保留了熟悉的编程模型：开发者标注哪些指针字段允许迁移，编译器把对这些字段的访问路由到 *guide*（引导符）。这确实要放弃两个假设（对象地址稳定、对托管对象做指针算术），并且适用于基于指针的结构而非任意代码；我们在 §7 明确划出这条边界。

我们用十种并发指针数据结构与六种最先进的回收/分层后端 [17, 44, 49, 60, 62] 评估 OBASE。使用键值存储负载（YCSB）与生产 trace（Meta、Twitter），我们表明 OBASE 在同等开销下把页利用率提升 2–4 倍、实现更高的内存节省（最多 70%）；反过来说，也能以可忽略的性能影响达到同等节省。在分层内存配置下，OBASE 用**一半的 DRAM 容量**即可达到相同吞吐。

---

## 2 动态对象重组的必要性

本节表明：热度碎片化在静态对象放置下既严重又不可避免。我们引入量化碎片化的指标，分析 Google 数据中心 trace [13]，说明对象热度随时间持续变化，最后解释为什么非托管语言让动态重组格外困难，从而引出 OBASE 的设计。

### 2.1 页利用率：一个指标

若某页在时间窗口 *T* 内至少被访问一次，OS 就把它标记为“活跃”。我们把**单页利用率**定义为被触及页中实际被访问容量所占比例。令 *P(T)* 为 *T* 内被触及页的集合，*U(p,T)* 为页 *p* 在 *T* 内被访问的**唯一字节数**，则聚合**页利用率**为：

$$\text{Page Utilization}(T) = \frac{\sum_{p \in P(T)} U(p,T)}{\sum_{p \in P(T)} \text{Size}(p)}$$

低利用率意味着：即便页中大部分字节是冷的，该页在 OS 看来仍是热的。已有工作 [21] 表明开源数据库（Redis、MongoDB）的页利用率很低：YCSB 负载下 75–90% 的被访问页中实际利用的容量不足 15%。

### 2.2 数据中心负载中的碎片化

为在更广泛的应用范围内理解热度碎片化，我们分析了 Google 六个数据中心工作负载的内存访问 trace [13]，由 DynamoRIO [12] 采集。我们逐个处理每次唯一访问，用 `/proc/self/smaps` 把页标注为 4KB 或 2MB，并统计每页被触及的唯一 64B 缓存行数。根据指令数、核数与典型的仓储规模 CPU 特征 [41]，我们估计这些 trace 捕获了约 30 秒的稳态执行。

图 2 展示了各工作负载单页利用率的累积分布：(a) 4KiB 与 2MiB 页聚合，(b) 仅 4KB 页，(c) 仅 2MB 页。

**图 2. Google 数据中心工作负载中的低单页利用率。** 六个应用的页利用率 CDF，左为所有页合计，中为 4KB 页，右为 2MB 页。

**聚合视图。** 两种页大小合并统计时，六种工作负载都出现大量利用不足的页。Yankee 与 Tahoe 碎片化最严重：中位利用率约 3%，即一半被触及页在冷数据上浪费了超过 95% 的容量。Bravo 次之，中位约 35%。其余工作负载整体利用率较高，但仍有长左尾：Sierra、Charlie、Whiskey 中约 25–30% 的页利用率低于 20%。若没有对象级信息，基于页的回收无法把这些低利用页与高利用页区分开。

**4KB 页。** 即便在最小页尺寸下碎片化依然可见：Tahoe 与 Yankee 约 80% 的页利用率低于 20%，Bravo 有 60% 低于 40%，Charlie、Whiskey、Sierra 有 35–40% 低于 40%。因此热度碎片化是访问模式的固有属性，而不只是大页造成的假象。

**2MB 页。** 巨大页上碎片化急剧恶化。Tahoe、Bravo、Yankee 极为极端：85–90% 的巨大页只利用了其 2MB 容量的 10% 以下；即便 Sierra 也有 40% 低于 20%。每一个这样的页消耗了基础页 512 倍的容量，却仅承载少量被访问的缓存行，旁边躺着超过 1.8MB 的冷数据。

> **发现 1：活跃页大多是冷的——OS 认为热的页中，70–90% 的字节从未被访问。**

### 2.3 对象热度随时间变化

针对热度碎片化的一个潜在方案是在**分配时**用静态提示或剖析 [40] 把冷热对象分离。该策略假设热度在分配时可识别、且之后相对稳定。

第一，在分配时识别热度就很难：同一条代码路径分配出的对象生命周期可能天差地别。例如 Redis 与 Memcached 中，同一个 SET 处理函数为所有到来的记录分配内存，但其中一条可能是每毫秒访问一次的会话令牌，另一条则是再也不会被碰的用户资料。静态分析无法在分配时区分它们。

第二，该方案假设对象热度相对稳定：分配时被判为热的对象将在整个生命周期内保持热。

我们表明这些假设并不成立。我们分析 Meta [22] 与 Twitter [65] 的对象级 trace（以逻辑时间记录，每桶 1000 万次操作）。图 3 可视化了访问总数排名前 500 万的键在 100 个桶上的访问强度。若热度是静态的，最流行的键（左侧）应呈现连续的竖直条带。但两条 trace 都显示出显著的**更替（churn）**：活动爆发之后是长期空闲间隔。

**Meta：分阶段热度。** Meta 负载呈现协同的相位变化——大量键同时变得不活跃，表现为白色水平条带。即便是整体上被反复访问的键，也会在活跃爆发与长期休眠间交替。一个当前装满热对象的页，会随着访问模式漂移最终变成充斥着冷对象的页。

**Twitter：零星热度。** Twitter 负载呈稀疏模式。一小片键持续热，但绝大多数（即便是前 500 万）表现为短暂爆发之间夹着长空闲。

为量化更替，我们测量复用距离的波动性（同一键两次访问之间操作数的 75 分位与 25 分位之差）。对中等大小对象（64B–4KB，占 Meta 键的 94%、Twitter 键的 98.2%），75% 的键复用跨度超过 5 倍，65% 超过 30 倍。也就是说，多数键的访问间隔波动超过一个数量级。

> **发现 2：热度是瞬态的——对象热度既无法在分配时预知，也不随时间稳定。因此一次性放置无法避免热度碎片化，必须基于变化的热度做动态对象迁移。**

### 2.4 非托管语言中的对象可移动性

动态移动对象在不同语言运行时中的难度不同。托管运行时（JVM、Go）可在 GC 期间通过原子更新引用迁移对象，移动相对容易。而 C++ 这类非托管语言中，程序**假定对象地址稳定** [23]，动态迁移因此显著更困难。故挑战不仅是动态重组布局，还要在别名与并发下不破坏正确性。

本文专门针对**非托管**语言实现对象可移动性，正因为这一挑战最难。我们表明地址空间工程在 C++（默认假设地址稳定）中可行——方法是让访问经由一层**间接（indirection）**而不是保持地址本身；在此设定下验证过的技术可自然推广到托管运行时。

为使问题在非托管场景中可控，我们聚焦**基于指针的数据结构**：元素通过指针间接访问。这类结构在内存密集型应用中无处不在（表 2）。我们不去拦截任意裸指针解引用（在 C++ 中不可行），而是把对可迁移对象的访问路由到显式的间接对象——**guide**（§3.2），应用用它代替裸指针。对象移动时，系统透明地把 guide 重定向到新位置。该机制必须在高度并发的环境中安全运行，且不能求助于粗粒度锁或 stop-the-world 暂停（那会抵消布局改进带来的性能收益）。§3 展示 OBASE 如何在 C++ 执行模型内提供这种透明性与并发性。

---

## 3 基于对象的地址空间工程

我们的目标是工程化应用的地址空间，使基于页的后端能高效回收或分层内存。OBASE 通过重塑对象放置实现：把热对象聚集到密集区域以提高页利用率，把冷对象隔离到独立区域以暴露大块可回收内存。OBASE 运行在“OS 以页为单位管理内存、且可能跨层迁移页”的环境 [20, 42, 43, 58]。设计不假设特定的分层策略或内存层级，只假设后端观察**每页活动性**。通过向后台呈现均匀热或均匀冷的区域，OBASE 让既有机制——从传统页回收（kswapd、zswap）到分层引擎（TMO、TPP、Memtis、HeMem）——既能做更好的决策，又无需变成对象感知 [43, 44, 49, 56, 62]。

用页级后端实现对象级放置有四项挑战：

- **C1 对象可移动性**：C++ 把对象身份与其地址绑定，迁移必须透明并在别名下安全；
- **C2 低开销跟踪**：热度分类必须跑在常见路径上且不降性能；
- **C3 动态适应**：热度持续变化（§2.3），布局必须持续演进；
- **C4 安全并发迁移**：线程仍持有指针时，对象也必须能移动，且不能全局暂停。

### 3.1 系统概览

OBASE 的设计基于把**布局**问题（组织对象，使页内数据冷热一致）与**分层**问题（决定哪些页被换出、放到哪里）分离。我们把解决布局问题的系统称为**前端**，把解决回收问题的称为**后端**：kswapd、TPP、Memtis、TMO 等都是假设布局已给定的后端。OBASE 是承担布局职责的前端，使任意后端都能在均匀冷热的页上工作。

**图 4. OBASE 概览。** OBASE 作为前端，把虚拟地址空间组织为热区与冷区。Object Collector 通过 Guides 与 SODA 监控访问，用 SAMA 迁移对象，并把重组后的地址空间呈现给 OS 后端。

图 4 展示了架构。OBASE 运行一个持续的控制闭环：

1. 对托管对象的所有解引用都经过轻量的 **guide** 抽象，记录该对象近期是否被访问；
2. 后台的 **Object Collector（OC）** 周期性处理这些元数据，按访问活动分类对象，并决定它们应位于 NEW、HOT 还是 COLD 堆；
3. 依据分类，OC 使用基于 **Active Thread Counts（ATC）** 的安全无锁协议在堆间迁移对象，重组地址空间；
4. 最后，OBASE 通过 **Spatially-Aware Memory Allocator（SAMA）** 把这些组织好的区域暴露给 OS：每个堆被布置为连续的虚拟地址范围，从而支持粗粒度的 OS 提示。

该闭环保证虚拟地址空间持续适应应用变化的工作集，同时向基于页的后端提供清晰的热/冷目标。

### 3.2 非托管语言中的对象可移动性

OBASE 用轻量的 **guide** 抽象把对象的逻辑身份与位置解耦。guide 携带对象的当前位置，以及 OBASE 需要的其他元数据。开发者用 guide 而非裸指针操作对象。guide 被解引用时，OBASE 解析出对象当前地址，并记录“该对象被访问”（见 §3.3）。这层间接正是使后续迁移透明的原因：原先操作指针的代码继续操作 guide。

开发者通过**标注**选择哪些指针可参与迁移（例如哈希桶中指向 key 和 value 的指针，或 B+ 树中的记录指针）。实践中标注量很小：对评估中的十种数据结构（表 2），每个托管类型只有 1–3 个可迁移指针字段（通常是结构的权威子指针），容器算法与客户端代码不动。

guide 由**三个编译器 pass** 强制实施（详见 §4.7）：

- **类型级 pass**：识别被标注的指针，把其解引用改写为调用 guide；
- **插桩 pass**：在访问点注入钩子，使 OBASE 无需修改应用逻辑即可观察使用；
- **校验 pass**：确保托管对象未被以不支持的方式使用（例如对节点做指针算术，或假设物理连续性）。

这些 pass 让开发者可以增量采用 OBASE，从对象驻留最关键的代码部分开始。

分工被刻意收窄：**开发者**标注少量指针字段，并保证没有未标注的裸指针别名到托管对象；**编译器**完成其余一切——改写声明与解引用、插入跟踪钩子。

被标注的 guide 像 `std::unique_ptr` 一样对其对象拥有唯一所有权：迁移时 OBASE 只更新这个唯一 guide，不跟踪隐藏别名，因此通过多个指针共享节点的结构（图、双向链表）不在支持范围内。校验 pass 会拒绝指针算术与对托管对象的物理连续性假设，而不是悄悄错误编译。由 guide 解引用得到的裸指针（如放在寄存器中）在所在公共操作的持续期内保持有效，因为该操作使对象的活跃线程计数保持大于零，从而否决任何并发迁移（§3.5）；调用方不得在操作之外缓存此类指针。

还有两条编程模型限制，用于在并发迁移下保持不变量。第一，`Guide<T>` 类型只出现在数据结构的实现内部，即 Pass 2 改写开发者标注的指针字段之处（§4.7），不出现在调用代码中。第二，guide 不作为参数或返回值跨越数据结构的公共边界：它们以内部字段形式持有，调用方通过原生类型 API 交互（例如图 7 中的 `HashMap::get(int idx)`）。两者共同保证：每次 guide 解引用都能追溯到某个公共方法入口，而该入口的 TAG 参与迁移协议。

### 3.3 低开销访问跟踪

要按温度对对象分类，OBASE 必须观察哪些对象在一段时间内被访问，而跟踪必须足够便宜以常驻运行。既有机制在两端都不合适：

- 硬件页表访问位以页为粒度，无法区分少量热缓存行与同页的兆字节冷数据 [8, 43]；
- 软件剖析器（DynamoRIO、LLVM 内存剖析）提供细粒度信息，但常驻部署开销不可接受 [12, 14]；
- 基于 PEBS 的硬件采样开销较低，但只提供统计覆盖，而非精确的逐对象跟踪 [7]。

OBASE 改为把访问跟踪**直接嵌入 guide 的指针解引用**，以极低开销获得对象级信息。guide 重载解引用运算符，使每次访问都记录底层对象被使用。现代 64 位架构在规范用户空间指针中保留高位，OBASE 便在这些未用位中存放少量逐对象元数据 [16]。内联元数据避免了外部侧表，并使“记录访问”成为普通指针使用的一部分。

元数据在每次解引用时通过**单条原子读—改—写（RMW）**更新：若已设置的访问位尚未置位则置位，从而跳过对频繁触及对象的后续更新，避免不必要的缓存一致性流量。这使常见路径很小，单次访问开销与一次缓存命中相当。元数据还保存运行时用于分类对象或协调迁移的状态，无需另行分配或间接访问。

为了让运行时无需遍历应用特定的指针图就能发现所有托管对象，OBASE 在进程堆上维护 **Sparse Object Data Activity（SODA）** 位图（§4.3）。SODA 记录哪些虚拟区域含托管对象，使 OC 能扫描这些区域来发现对象，而不必理解容器内部结构。

### 3.4 动态工程化布局

§3.3 的访问信号输入到一个持续重组虚拟地址空间的布局策略。OBASE 按观测温度对对象分组，使基于页的后端看到大块均匀区域，而不是冷热交错的数据。三个逻辑温度堆承担这一职责：

- **NEW**：新分配、访问模式尚不明确的对象；
- **HOT**：当前工作集；
- **COLD**：多个扫描周期内不活跃的对象。

每个堆占据专用虚拟地址范围，因此对象地址直接编码其温度。对象随访问强度变化在堆间移动（图 5）：不活跃的 HOT 对象被降级到 COLD，重新变活跃的 COLD 对象被升回 HOT。

**图 5. 对象迁移状态图。** 对象依据访问强度在堆间迁移。OC 把被访问对象提升到 HOT，把不活跃对象降级到 COLD，使 SAMA 能按需对不同区域施加不同的 madvise 策略。

这种持续重分类跟踪的是负载不断演进的工作集，而非其分配历史。为在虚拟地址空间中落实这些逻辑堆，OBASE 使用 **Spatially-Aware Memory Allocator（SAMA）**：为每个堆预留连续虚拟地址范围，并在其中再分配对象。连续性是有意的设计选择，使粗粒度 OS 提示可作用于整个堆而非单页，从而把大块冷内存暴露给基于页的分层系统。

OC 周期性扫描 SODA 对每个托管对象分类。自上次扫描以来被触及的对象是提升候选；持续未被触及的对象累积冷度证据。对单个不活跃窗口做反应会让分类对瞬时爆发敏感，因此 OBASE 为每个对象维护**连续不活跃窗口（Consecutive Inactive Window, CIW）**计数器：每个无访问的扫描窗口使 CIW 加一；任何访问都把它清零。CIW 超过冷阈值的对象才可被降级到 COLD；COLD 中被访问的对象回到 HOT。这种滞回（hysteresis）保证只有**持续**不活跃才触发迁移。

冷阈值 $C_t$ 决定对象必须不活跃多久才被迁移。过于激进则 COLD 对象被频繁重新访问；过于保守则本可回收的内存在 HOT 中滞留。OBASE 用**提升率（promotion rate）**目标自适应调整 $C_t$。提升率 PR 定义为每个扫描窗口中工作集里来自 COLD 堆的比例：

$$PR = \frac{\text{unique COLD pages accessed}}{\text{working set size}} \times \frac{60}{\text{scan interval (s)}}$$

关键在于，OBASE 测量 COLD 堆访问时**不关心这些页物理上位于哪一层**——它无法判断页所在层级，也不尝试判断。若观测率超过目标，$C_t$ 加一个窗口；低于目标则减一。这一加性调整会收敛到负载特有的工作区间。目标不是最小化 COLD 堆访问，而是确保被归为 COLD 的页对任何后端策略来说都是**安全的回收目标**。§4 详述初始化与调参。

设计上 OBASE 与任何具体后端解耦。基座系统只重组地址空间，不发出回收提示。这种分离是有意的：地址空间工程的价值在于让页均匀冷或热，从而提高任何基于页机制的精度。kswapd、TMO、TPP、Memtis 等后端通过其既有接口（PTE-A 位、PEBS 采样或 PSI 信号）观察每页活动性，当 COLD 页只含冷对象时自然做出更好的决策。

可选地，OBASE 可发出**主动提示**以加速回收。在此“提示模式”下，一旦提升率稳定低于目标，OBASE 就以 `MADV_COLD` 或 `MADV_PAGEOUT` 标记 COLD 页，表明它们可安全回收。类似地，SAMA 可为 HOT 请求巨大页（`MADV_HUGEPAGE`），以在密集热数据上选择性改善 TLB 覆盖率。这些提示严格是建议性的，用于补充而非替代后端策略。

### 3.5 安全的并发迁移

重组地址空间需要在应用线程可能仍持有指针时搬移对象。托管语言用读屏障（load barrier）或 stop-the-world 暂停解决；C++ 两者皆无：没有运行时拦截指针加载，生产系统也无法容忍暂停。因此 OBASE 必须在**不阻塞线程**的前提下迁移对象，同时保证每次解引用都看到有效位置。

如 §3.2 所述，所有解引用都经过 guide，因此迁移简化为原子地更新一个指针大小的字。调用方永远不会跟随过期指针，也不需要显式同步。

**轻量级作用域（Lightweight Scoping）。** 只有当没有线程在使用某对象时才可迁移它。经典做法——stop-the-world 暂停或逐访问读屏障——不适用于 C++ 系统代码 [30, 50, 63]。OBASE 引入逐对象的 **Active Thread Count（ATC）**，跟踪在迁移窗口内有多少公共数据结构操作观测到了该 guide。ATC 刻画“正在被使用”：若线程开始一个可能解引用该对象的操作，则在该作用域内为 ATC 加一；操作完成时减一。只有 ATC 归零时——表明没有线程正在执行可能读写该对象的操作——才允许迁移。

ATC 必须高效更新。若在每次指针解引用时递增 ATC，开销不可接受；OBASE 把跟踪**限定在公共 API 边界**。线程进入数据结构操作（如 `get`、`insert`）时向线程本地的 **Active scope guard（TAG）** 注册，操作完成时该 guard 对所有被触及对象递减 ATC。这符合 C++ 程序员对指针有效性的理解：指针在其取得它的那次操作持续期内有效，而非无限期有效。编译器插桩（§4.2）自动插入必要的钩子。

**图 6. 应用线程执行流与 OC 的交互。** 左侧为插桩后的调用图，代码在三个位置插入。公共函数创建并销毁线程本地 Active scope Guard（TAG），维护嵌套层数并把线程注册到 Thread Activity Index（TAI）中。仅当 guide 被加入 TAG 时才递增活跃引用计数。引用计数只在最外层公共函数退出时递减。ATC 仅在 PREPARE 与 ACTIVE 状态下启用。

**通过 epoch 实现活跃窗口。** 始终开启 ATC 跟踪即使在无迁移进行时也会带来开销，故 OBASE 只在周期性迁移 epoch 期间激活 ATC。窗口之外，guide 解引用只记录访问活动（§3.3），没有 ATC 开销。OC 发起迁移时协调一次 epoch 转换，确保在任何对象被移动前所有活跃线程都已启用 ATC 跟踪。基于 epoch 的激活把同步成本限制在短暂、不频繁的窗口内。

**乐观迁移（Optimistic Migration）。** 在 ATC=0 时，OBASE 使用受乐观并发控制启发的协议搬移对象：OC 把对象复制到目标堆，然后尝试原子地把 guide 切换到新地址。若复制期间有任何线程访问该对象，提交失败、迁移被放弃——对象留在原处，线程看到有效数据。该乐观方法有两个关键性质：(1) 线程从不在迁移上阻塞；(2) 并发访问能安全地**否决**重定位，而不是观察到不一致状态。§4.5 详述基于 CAS 的协议。

**安全性与非阻塞进展。** 安全性来自单一不变量：guide 被原子更新，且任何冲突访问都会中止迁移。没有线程会跟随过期指针。进展是非阻塞的：应用线程从不等候收集器，收集器对每个对象只做有界的工作再提交或放弃。频繁访问的对象天然抵抗迁移（其 ATC 很少归零），而冷对象最终会移动。结果是一个无需 stop-the-world 暂停、持续重塑的地址空间，同时保留熟悉的指针语义。

---

## 4 实现

本节介绍 OBASE 的具体实现：guide 元数据编码（§4.1）、作用域 guard 数据结构（§4.2）、SODA 位图布局（§4.3）、控制器参数（§4.4）、基于 epoch 的迁移协议（§4.5）、内核回收优化（§4.6）以及自动化 guide 管理的编译器 pass（§4.7）。

### 4.1 Guide 元数据编码与堆分配

每个 guide 使用 x86-64 规范用户空间地址所需的 48 位，把高 16 位改用于元数据：

| 字段 | 位数 | 含义 |
|---|---|---|
| ATC | 7 | 每对象最多 128 个并发线程 |
| CIW | 5 | 最多 32 个窗口（默认 120s 间隔下约 60+ 分钟） |
| 堆标识 | 2 | 当前堆（NEW / HOT / COLD） |
| 标志 | 2 | 访问位与迁移锁位 |

随着 128 位寻址日益普及 [5]，实现可扩展这些字段而无需改变 guide 抽象。我们用基于 jemalloc extent 管理构建的 **SAMA** 实现三个堆：为每个堆预留大的 mmap 区域并在其中分配对象，在对象迁移或被释放时把空闲 extent 归还 OS。

### 4.2 高效的作用域 guard 跟踪

图 6 展示插桩后的调用图。编译器插入三个钩子：

- `createTAG`（绿色）：公共 API 入口；
- `addToTAG(guide)`（紫色）：每次 guide 解引用前；
- `destroyTAG`（黄色）：公共 API 出口。

公共函数可能调用同样会解引用 guide 的私有辅助函数；TAG 跟踪所有这些访问，但只在**最外层**公共函数返回时递减 ATC。这种嵌套感知设计保证整个操作期间 ATC 保持为正，即便内部辅助函数被内联或被多次调用。

我们用 **BaseDeltaPtrSet** 实现 TAG，它利用指针局部性：把指针编码为基址加 32 位 delta，把至多 16 个邻近指针放进两个缓存行。当指针落入已有组时插入为 O(1)；否则创建新组为 O(log *G*)，*G* 为组数。由于单次操作内的指针聚集紧密（如同一桶的 key 与 value、树路径上的节点），多数插入命中已有组。在评估的十种数据结构中，每次操作触及的唯一 guide 数中位数从 3（哈希表）到 12（B+ 树遍历），使每操作 TAG 开销低于 100 ns。

### 4.3 SODA 位图结构与对象发现

**Sparse Object Data Activity（SODA）** 位图采用两级结构覆盖堆地址空间，而不为空区域分配存储。SODA 把地址空间划分为粗粒度块；每块包含若干 64 位字，其中每个位表示固定大小槽位上是否存在 guide。块按需惰性分配，为空时回收。由于 SODA 跟踪的是 **guide 指针**而非对象地址，它在重定位后依然有效——对象移动时 guide 的地址不变。内存开销为每个潜在 guide 槽位 1 位，外加块级簿记。

OC 以可配置间隔扫描 SODA（默认 120 s），该间隔与仓储规模分层系统中的冷内存检测阈值对齐 [29, 43]。

### 4.4 冷阈值控制器

OBASE 把 $C_t$ 初始化为 3 个扫描窗口；在默认 120 s 间隔下约为 6 分钟不活跃才降级，与分层存储的“五分钟规则”一致 [19, 32, 33]。目标提升率 1% 取自生产环境压缩内存与 CXL 分层部署所用的预算 [29, 43]。$C_t$ 被限定在 1 到 32 个窗口之间。每个扫描窗口后，加性增/加性减（AIAD）循环（算法 1）把观测提升率 $PR_{actual}$（§3.4）与目标比较，并相应调整 $C_t$，从而使阈值稳定到负载特有的区间而不出现大幅震荡。

**算法 1　冷阈值控制器（每个扫描窗口）**

```
1: PR ← unique COLD pages accessed / working-set size × 60 / scanInterval
2: if PR_actual > PR_target then
3:     C_t ← min(C_t + 1, 32)      ▷ 降级被过于频繁地重访 → 更保守地降级
4: else if PR_actual < PR_target then
5:     C_t ← max(C_t − 1, 1)       ▷ 回收更激进  ▷ 预算有余量
6: end if
```

### 4.5 Epoch 协议与乐观迁移

如图 6 所示，OC 使用全局 epoch 计数器与 **Thread Activity Index（TAI）** 协调迁移，经历三个状态（INACTIVE、PREPARE、ACTIVE）。TAI 是每线程槽位，使 OC 能在不阻塞线程的情况下验证每个活跃线程都已观测到新 epoch。

**Epoch 转换。** 在 INACTIVE 状态下 ATC 更新被禁用，解引用只设置访问位。OC 发起迁移时递增 epoch 计数器并进入 PREPARE 状态 ①。进入公共方法的线程把当前 epoch 记录到 TAI（按线程 ID 哈希索引的小数组）；退出时清空槽位。OC 反复扫描 TAI；一旦所有非空槽位都反映新 epoch ②，说明所有活跃线程都已启用 ATC 跟踪，OC 进入 ACTIVE 状态。迁移完成后 OC 回到 INACTIVE ③。关键在于线程从不阻塞：它们只记录 epoch 参与情况，收敛检查由 OC 完成。

**乐观数据迁移。** 在 ACTIVE 状态内，OC 用 **Optimistic Data Migration（ODM）** 协议（算法 2）迁移对象，其精神类似乐观并发控制（OCC）——先执行任务再验证，本质上是一个弱事务。对 ATC=0 且迁移锁已清除的候选对象，OC：(1) 在 guide 槽位上原子地设置迁移锁位；(2) 通过 SAMA 在目标堆分配空间并复制对象；(3) 构造新 guide（新地址、堆 ID、重置 CIW、清除锁），用单次提交 CAS 发布——同一个原子操作既安装新地址又释放锁。若提交 CAS 失败，说明有并发解引用改变了 guide，迁移被放弃（OC 显式清除它设置的锁，见算法 2）。

**算法 2　乐观数据迁移（ODM）**

```
1: procedure MigrateObject(ptr, targetHeap)        ▷ ptr: guide 槽位地址
2:     guide ← AtomicLoad(ptr)                     ▷ 读取对象 guide
3:     if RefCount(guide) > 0 then
4:         return false                            ▷ 对象正在使用
5:     end if
6:     srcAddr ← ExtractAddr(guide)
7:     srcHeap ← ExtractHeapType(guide)             ▷ 当前堆类型
8:     SetMigrationLock(ptr)                       ▷ CAS；标记为迁移中
9:     size ← GetSize(srcAddr)
10:    dstAddr ← samalloc(size, targetHeap)
11:    if dstAddr = null then                       ▷ 分配失败
12:        ClearMigrationLock(ptr)
13:        return false
14:    end if
15:    Copy(dstAddr, srcAddr, size)                 ▷ 复制数据
16:    newGuide ← CreateGuide(guide, dstAddr, targetHeap)   ▷ 继承 guide 元数据中的锁
17:    ClearMigrationLock(newGuide)                 ▷ 本地清除继承的锁；下面通过 CAS 提交
18:    success ← AtomicCAS(ptr, guide, newGuide)
19:    if success then
20:        Free(srcAddr, srcHeap)                   ▷ 释放旧内存
21:        return true
22:    else
23:        Free(dstAddr, targetHeap)                ▷ 回滚
24:        ClearMigrationLock(ptr)                  ▷ CAS；释放我们设置的锁
25:        return false                             ▷ 迁移失败，guide 已变
26:    end if
27: end procedure
```

**竞争消解。** 表 1 说明迁移与访问之间的竞争。每次解引用都执行一个原子 CAS，它设置访问位并**清除**迁移锁位。

| 时间 | 角色 | 动作 | ATC | 锁 | 结果 |
|---|---|---|---|---|---|
| **(a) 并发解引用中止迁移** | | | | | |
| $t_0$ | OC | 读 guide | 0 | 0 | 可迁移 |
| $t_1$ | OC | CAS：置锁 | 0 | 1 | 开始复制 |
| $t_2$ | 线程 | 解引用 | 1 | 0 | 锁被清除 |
| $t_3$ | OC | CAS：提交 | | | **不匹配 → 中止** |
| **(b) 无干扰，迁移成功** | | | | | |
| $t_0$ | OC | 读 guide | 0 | 0 | 可迁移 |
| $t_1$ | OC | CAS：置锁 | 0 | 1 | 开始复制 |
| $t_2$ | OC | CAS：提交 | 0 | 0 | 成功 |

**表 1. 迁移与并发访问之间的竞争。** (a) $t_2$ 的解引用修改了 guide，使 OC 的提交 CAS 失败。(b) 若无线程介入，两次 CAS 均成功，对象被移动。

若线程在 OC 复制对象 *x* 时（$t_2$）访问它，guide 改变、提交 CAS 失败（$t_3$），*x* 留在原地址。线程始终看到有效数据；并发访问优先于迁移。

### 4.6 内核页回收优化

当对象迁移到 COLD 堆且 OC 发出 `MADV_PAGEOUT` 后，Linux 的页回收路径成为高效内存分层的瓶颈。`shrink_folio_list()` 的默认路径逐页处理：清除页表项（PTE）、发起 TLB flush 或 shootdown（向每个核发送处理器间中断 IPI）、把页提交给块 I/O 层。回收大区域时这种细粒度做法开销可观——逐页 TLB 失效与 IPI 的累积成本，即使对只访问无关热数据的线程也会拖慢性能。

我们修改 `shrink_folio_list()` 以批量处理：聚合页（最多一个覆盖 512 个基础页的完整 PMD），逐页清除 PTE 并标记待换出，但把 TLB 失效推迟到整批准备好之后，然后在整个范围上发出一次 flush 并批量提交 I/O。通过摊销 TLB shootdown 与 I/O 提交，与未修改内核相比，降级 10 GiB 内存时 IPI 减少 **99% 以上**。

除本地分层外，批量失效还解决了基于 RDMA 的远端内存系统 [35, 48, 57] 的可扩展性瓶颈：那里逐页 shootdown 造成 IPI 风暴，其延迟随线程数超线性增长；跨数百页聚合使回收路径能同时为 CXL 与 RDMA 后端扩展。

### 4.7 面向 guide 管理的编译器 pass

OBASE 在 Clang 与 LLVM 上实现三个互补的编译器 pass，把裸指针转换为 guide 并插入 TAG/ATC 插桩，把开发者工作量限制在“标注哪些指针字段被托管”。

**可见性提取（Pass 1）。** 一个 Clang 前端 pass 用 `RecursiveASTVisitor` 找出作为数据结构入口的公共方法（如 `get`、`set`、`delete`）并记录其可见性，供后续阶段使用，从而保证 TAG 只在公共边界创建。

**指针到 guide 的转换（Pass 2）。** 第二个 Clang pass 用 rewriter 把开发者列出的指针声明与使用转换为 guide，从而精确控制 OBASE 托管哪些对象。

**IR 插桩（Pass 3）。** 第三个 pass 运行在 LLVM IR 上。不动点分析首先确定直接使用 guide 的函数集合（通过转换、析构调用、运算符重载或赋值），再沿调用图传播以找出间接触及 guide 的函数，并结合 Pass 1 的可见性数据对每个函数分类。然后它在触及 guide 的公共函数的入口与出口插入 `createTAG`/`destroyTAG`，并在所有被转换函数（公共与私有）的每次 guide 解引用前插入 `addToTAG`。这样把 TAG 创建限制在公共边界，同时仍跟踪整个调用栈上的每次访问（图 7）。

**图 7. 编译器变换。** 开发者标注 `HashMap` 的一个指针字段；pass 把其类型从 `char*` 改写为 `Guide<char>`，用 `createTAG`/`destroyTAG` 包住公共方法，并在每次 guide 解引用前插入 `addToTAG`。`buckets` 数组未被标注、仍留在内存中，因此索引它无需守卫；只有 guide 解引用（`*v`）需要。调用方以原生 key 调用 `HashMap::get`（没有 guide 跨越公共方法边界），故调用方代码既不持有也不解引用 guide。`addToTAG` 内的 ATC 递增仅在 TAG 状态非 INACTIVE 时发生。

```cpp
 1  void HashMap::get(int idx) {
 2      createTAG();                             // 公共入口
 3      Guide<char>& v = buckets[idx];           // HashMap 成员
 4      addToTAG(&v);                            // 解引用前
 5      if (*v > 37) { destroyTAG(); return; }
 6      addToTAG(*v = 42;)                       // 原文如此
 7      addToTAG(&v);
 8      std::cout << "Value: " << *v << std::endl;
 9      destroyTAG();                            // 公共出口
10  }
```

---

## 5 评估

我们从四个维度评估 OBASE：

- **E1 有效性**：OBASE 能否在不同数据结构与负载下降低热度碎片化与内存占用？（§5.2）
- **E2 后端协同**：OBASE 能在多大程度上提升既有基于页的回收与分层后端的有效性？（§5.3）
- **E3 开销与可扩展性**：跟踪与迁移引入多少运行时开销？它如何随线程数伸缩？（§5.4）
- **E4 动态行为**：在长时间运行的真实负载中，OBASE 如何适应变化的热集？（§5.5）

### 5.1 实验设置

所有实验在 Intel Xeon Gold 5218（16 核，关闭 SMT）上进行，performance governor，Ubuntu 22.04，Linux 内核 6.12。内存子系统分两层：快层为 2×16 GB DDR4-2400 DRAM；慢层为 4×128 GB Intel Optane DC Persistent Memory 100（2666 MHz）。六个内存设备占据不同通道以避免接口冲突。我们把 Optane PMEM 配置为 Memory Mode 并作为独立 NUMA 节点暴露，形成代表新兴 CXL 挂载内存部署的两层结构 [46, 49]。Optane 层访问延迟约为本地 DRAM 的 2.5 倍 [64]，与第一代 CXL 内存特征一致 [58]。需要 swap 的回收实验使用 512 GB Intel P4800X Optane SSD。

**CrestDB 测试床。** 我们实现并发内存键值存储 CrestDB，以在多种数据结构与并发机制上评估 OBASE。CrestDB 集成了十种高性能数据结构，涵盖并发控制谱系（表 2），从无锁算法到全局锁。许多结构借用自提供生产级实现的 ASCYLIB [34]。这种多样性表明 OBASE 的编译器插桩与迁移协议无需结构特定修改即可兼容广泛的同步方式。所有数据结构以 guide 指向 key 与 value 对象；CrestDB 对插入数据做深拷贝，确保 OBASE 管理的是权威副本而非应用持有的别名。除特别说明，CrestDB 运行 6 个服务线程与 6 个客户端线程。

| 结构 | 并发控制 | 使用方 |
|---|---|---|
| HashTable Harris [36] | 无锁算法 | NGINX |
| HashTable Pugh [54] | 细粒度读写锁 | Redis、Memcached |
| HashTable Java CHM [15] | 分段桶锁 | Linux 内核、HAProxy |
| SkipList Coarse | 全局锁 | LevelDB/RocksDB |
| SkipList Fraser [31] | 无锁算法 | Redis Sorted Sets |
| SkipList Herlihy [37] | 乐观细粒度 | Cassandra、CockroachDB |
| B+Tree Coarse | 全局锁 | SAP HANA |
| B+Tree OCC | OCC + epoch 回收 | VoltDB 索引 |
| MassTree [47] | OCC + RCU | MICA、Silo |
| Adaptive Radix Tree [45] | 细粒度读写锁 | DuckDB、PostgreSQL |

**表 2. 用于评估 OBASE 的并发数据结构。** 覆盖无锁、细粒度与粗粒度并发机制，说明 OBASE 兼容多样的同步方式。

**负载。** 使用带 Zipfian 分布键的 YCSB 套件模拟生产负载典型的偏斜访问模式。为评估真实适应性，我们重放来自 Meta（CacheLib [22]、DBbench [24]）与 Twitter（Cluster 7、Cluster 23）[65] 的生产 trace。

**控制器参数。** 除特别说明，OC 每 120 s 扫描一次，目标提升率 1%，每次扫描把 $C_t$ 调整 ±1 个窗口且保持 $1 \le C_t \le 32$（§3.4）；保守的 1% 目标沿用生产压缩内存部署 [43, 62]。最优速率依赖硬件（更快的层可容忍更高的冷访问比例），硬件感知调参属于未来工作。§5.5 考察 $C_t$ 随时间的变化。

### 5.2 有效的地址空间工程（E1）

我们首先评估 OBASE 是否达成核心目标：降低热度碎片化、把不可回收的冷数据转化为可回收页。我们以全部十种数据结构运行 CrestDB，负载为带 Zipfian 键的 YCSB A、B、C。装载 1000 万个键，键 30 字节、值 1024 字节，数据集 13 GiB。除特别说明，本节结果均在 OBASE 收敛后（提升率低于 1% 目标）测得。

**图 8. OBASE 的有效性（YCSB，1000 万键）。** 上：相对于基线分配器的页利用率提升；OBASE 在各负载与数据结构上把利用率提升 2–4 倍。下：OBASE 把 COLD 堆换出（经 Kswapd）后的 RSS 降低；内存占用缩小 65–72%。

**页利用率。** 图 8(a) 报告 OBASE 把对象重组进 NEW、HOT、COLD 堆前后的页利用率。起初各数据结构在 120 s 窗口上测得利用率为 18–20%：多数页只含少量被访问的缓存行，正反映生产 trace 中观察到的热度碎片化（§2）。经过三个扫描间隔后，OC 依据 guide 访问位对对象分类并迁移到 HOT 或 COLD 堆。与基线相比，OBASE 把页利用率提升：工作负载 A（50% 写）约 2 倍，B（5% 写）约 3 倍，只读工作负载 C 最高 4 倍。跨数据结构，收敛后的绝对利用率从约 40%（A）到 80%（C）。负载间差异反映 NEW 与 HOT 堆的相互作用：工作负载 C 无更新，一旦对象被判为热就留在 HOT 堆、不再有新对象分配到那里，几乎所有被访问字节都密集地落在少量 HOT 页中，利用率接近 80%；工作负载 B 偶有更新把新值分配到 NEW，工作集分散在 NEW 与 HOT 之间，利用率稳定在 60–70%；工作负载 A 频繁更新、持续向 NEW 注入新对象，利用率仍大致翻倍，但达不到只读水平，因为较大部分热对象在初始 epoch 内仍居于 NEW。

提升在十种结构各异的数据结构上的一致性表明：OBASE 的收益源于对象温度聚类，而非结构特定的布局优化。

**内存占用。** 更高的页利用率直接转化为可回收的冷内存。一旦提升率低于 1% 目标——YCSB 下通常在 3–4 个扫描间隔（6–8 分钟）后——OBASE 对 COLD 堆主动发出 `madvise(MADV_PAGEOUT)`。图 8(b) 显示相对不做回收的基线的 RSS 降低：在所有数据结构与负载上，OBASE 把 RSS 降低 **65–72%**。工作负载 B 装载 1000 万键时基线用 12.4 GiB；OBASE 收敛并换出 COLD 后 RSS 降至 3.5–4.0 GiB。由于 COLD 页几乎只含不活跃对象，主动换出不会造成 swap-in 风暴或明显吞吐下降（开销见 §5.4）。

> **要点 1：OBASE 在不需要了解各结构语义的情况下跟踪对象热度，从而在**全部**数据结构上提升页利用率，在多样的并发机制上一致地降低热度碎片化。**

### 5.3 后端协同（E2）

§5.2 的内存节省只有在 OS 分层后端能利用它们而不牺牲性能时才有价值。下面表明 OBASE 使既有回收与分层系统在保持吞吐的同时实现激进的内存节省。

**图 9. OBASE 与回收后端（YCSB-C，MassTree）。** 上：收敛后的 RSS。下：吞吐。无 OBASE 时后端面临内存节省与性能的权衡；有 OBASE 时（斜条纹柱），所有后端都在几乎不损失吞吐的情况下达到接近最优的内存节省。

#### 5.3.1 基于换页的回收后端

我们以 MassTree 在 YCSB-C 上运行 CrestDB，采用内存受限配置：工作负载占用 13 GiB，但活跃工作集约 4 GiB。比较四种回收策略，各有/无 OBASE 前端：

- **Kswapd**：Linux 后台回收器，由同机进程引起的内存压力触发；
- **Cgroup**：把内存上限设为工作集大小（4 GiB），强制激进回收；
- **TMO**：Meta 基于 PSI 的主动回收器 [62]；
- **OBASE Hinted**：收敛后对 COLD 堆主动 `MADV_PAGEOUT`。

评估中 OBASE 单指前端本身（地址空间重组，不启用主动换页），因此“OBASE+TMO”这类组合把重组带来的收益与提示完全隔离；OBASE Hinted 则额外对 COLD 堆发 `MADV_PAGEOUT`。

图 9 揭示了 OBASE 化解的根本性权衡。无 OBASE 时后端必须在内存效率与性能间二选一：

- Kswapd 把 RSS 从 13 GiB 降到 7 GiB（1.8 倍）但无吞吐损失，不过由于它只通过 PTE 扫描看到页级视图，仍有 3 GiB 冷数据被困在混温页中；
- Cgroup 达到 4 GiB（3.2 倍），但吞吐崩塌 38%，因为内核不可避免地驱逐热对象；
- TMO 达 6.5 GiB（2 倍）且无吞吐损失，但无法进一步回收，因为 PSI 只在页级反映压力，无法体现对象级冷度。

有 OBASE 时，所有后端都达到 4 GiB RSS——与最激进策略持平——且无吞吐退化：Kswapd 现在优先回收 COLD 页；Cgroup 不再抖动，因为被驱逐的页确实只含冷对象；TMO 的 PSI 探测不再遇到抵抗回收的混温页。OBASE Hinted 不依赖任何后端策略，主动取得同样结果。

> **要点 2：OBASE 在保留应用性能的同时实现激进回收。**

#### 5.3.2 分层后端

为展示降低热度碎片化对内存分层的收益，我们以三种基于页的迁移系统评估 OBASE：TPP [49]、AutoNUMA [60]、Memtis [44]。

**设置。** 以 MassTree 装载 CrestDB，5000 万键（键 30 字节、值 1024 字节），数据集 67 GiB。DRAM:CXL 比在三种配置间变化：1:4（14.8 GiB DRAM）、1:8（7.4 GiB）、1:16（3.9 GiB），其余容量在 Optane PMEM 上。与 [44] 一致，性能归一化到“全部数据位于慢层”的基线；大于 1.0× 表示有效使用 DRAM 带来的加速。

**图 10. OBASE 与分层后端（YCSB-B，MassTree，5000 万键）。** 吞吐归一化到仅 CXL 基线（越高越好）。无 OBASE 时，DRAM 缩小导致热集放不下、性能下降；有 OBASE 时热集被压缩，即便在 1:16 下也能保持稳定性能。

**热集错配。** 无 OBASE 时，工作集跨 16.3 GiB 的页、利用率 21%——略大于 1:4 的 14.8 GiB DRAM 预算，任何配置都无法把所有被访问数据留在 DRAM。有 OBASE 时，同样的逻辑工作集压缩到 6.33 GiB、利用率 57%，在 1:4 与 1:8 下都游刃有余。

**TPP。** TPP 用基于滞回的提升与主动降级来管理 DRAM 余量。图 10 显示 TPP 在 1:4 取得 1.65× 加速，到 1:16 时随着 DRAM 预算小于碎片化热集而降为 1.25×。有 OBASE 时 TPP 在 1:4 达 1.85×，1:16 仍保留 1.45×——在最紧的配比下提升 16%。

**AutoNUMA。** AutoNUMA 立即提升任何被访问的远端页、无滞回，热集超出 DRAM 容量时会抖动。无 OBASE 时它比 TPP 差 15–20%，1:16 时仅 1.05×，勉强好于仅 CXL。有 OBASE 时其性能与单独的 TPP 持平：1:8 下 OBASE+AutoNUMA（1.6×）超过单独的 TPP（1.55×）。当页均匀冷或热时，即便朴素的提升决策也变得正确。

**Memtis。** Memtis 用硬件采样（PEBS）识别热页，基线结果最好：1:4 达 1.8×，1:16 达 1.55×。有 OBASE 时改善到 1.95× 与 1.7×。增益较小（3–10%，而 TPP/AutoNUMA 为 12–29%），因为 Memtis 本已捕获大量页级信号——但即便最复杂的页级策略也能从对象级重组中获益。由于 OBASE 把有效热集缩小 2.5 倍，1:X 的配比表现与基线 1:(X/2) 相当。例如 OBASE+TPP 在 1:16 达 1.45×，与单独的 TPP 在 1:8（1.55×）仅差约 6%。对运维者而言，这意味着同样的性能只需一半 DRAM，或将现有多层部署的有效容量翻倍。

> **要点 3：OBASE 压缩热集使其适配更小的 DRAM 预算，让基于页的分层后端真正有效。**

### 5.4 开销与可扩展性（E3）

#### 5.4.1 稳态开销

图 11(a) 报告在没有回收或分层后端激活时，相对未插桩基线的吞吐与 p90 延迟开销。平均而言，OBASE 使吞吐降低 **2.5%**、p90 延迟升高 **5%**。哈希表影响最小（吞吐降 1.5–3%），跳表、B+ 树与 ART 为 3–5%。差异与每次操作触及的 guide 数相关：哈希查找解引用很少节点，树遍历访问更多节点、按比例承担更多跟踪开销。开销主要有两个来源：(1) 每次 guide 解引用上的 tagged-pointer 读—改—写（4–5 ns，与 L1 缓存命中相当）；(2) ACTIVE 迁移 epoch 期间的 TAG/ATC 簿记。OC 运行在专用线程上，占用不到 1% CPU 时间。

**图 11. OBASE 的开销与可扩展性（YCSB，1000 万键）。** 上：各数据结构的吞吐与 p90 延迟开销，范围 1.5–5%。下：2 到 32 线程的可扩展性；柱为绝对吞吐，标记为相对基线的开销；开销稳定在 1–8% 且无上升趋势。

#### 5.4.2 线程可扩展性

一个自然的担忧是 OBASE 的原子操作在高线程数下是否成为竞争瓶颈。我们在三种代表性数据结构上把 CrestDB 服务线程从 2 变到 32 以测量可扩展性：Hashtable Pugh（细粒度锁）、Skiplist Fraser（无锁）、MassTree（OCC + epoch 回收）。

图 11(b) 显示吞吐随线程扩展，开销在任何线程数下都稳定在 **1–8%**。关键的是开销不随并发上升：三种结构在 2 与 32 线程下开销相近。这源于设计：guide 元数据更新针对每对象状态（无跨线程竞争）、test-and-set 优化跳过对热对象的冗余写入、TAG 是线程本地的、ATC 递增只在短暂 ACTIVE epoch 内发生。

这 2–5% 开销是相对“无内存压力的纯 DRAM 基线”测得的理想情景，生产系统很少享有。在 OBASE 所面向的分层内存环境中比较结果会反转：如 §5.3 所示，没有 OBASE 的后端因页选择不佳损失 10–38% 吞吐，有 OBASE 的后端则达到纯 DRAM 性能。

> **要点 4：OBASE 在各数据结构上带来 1.5–5% 开销，2 到 32 线程内保持在 8% 以内——相对于 §5.2–5.3 的后端收益，代价温和。**

### 5.5 真实 trace

合成负载只施加受控形式的偏斜，而生产系统行为复杂得多：热集漂移、读写删比例混杂、局部性模式在数小时内演进。因此我们在四条真实 trace 上评估 OBASE，考察降碎片化收益能否超出 YCSB 泛化，以及反馈控制器能否稳定适应长期访问模式变化。四条 trace 覆盖：

- **Meta CacheLib [22]**：读密集（83% GET），流行键逐渐漂移；
- **DBench Mixgraph [24]**：模拟 Meta 的 ZippyDB，跨 30 个前缀的键区间局部性；读密集（85% GET、14% PUT、1% SEEK）；
- **Twitter Cluster 7 [65]**：高偏斜（$\alpha = 1.07$），读与写的工作集小而集中；
- **Twitter Cluster 23 [65]**：写密集（31% SET、30% INCR），低偏斜（$\alpha = 0.274$），含删除。

这些 trace 覆盖从高度偏斜（Cluster 7）到近乎均匀（Cluster 23）、从读主导（CacheLib）到写密集（Cluster 23）的谱系。我们在 CrestDB（ART）上重放每条 trace，测量内存降低与页利用率提升。

**页利用率（E1）。** 各 trace 上页利用率提升 1.8–3.4 倍。Cluster 23 增益最高（3.4 倍），因其低偏斜把访问分散到大量键上、基线利用率极低；Cluster 7 的高偏斜天然集中访问，基线本已合理，相对提升较小（1.8 倍）。

**图 12. 生产 trace 结果。** 内存降低（左轴）：OBASE Hinted vs 不回收、OBASE+TMO vs 仅 TMO。页利用率提升（右轴，斜条纹）来自地址空间重组。

**内存降低（E1、E2 与 E4）。** 图 12 显示 OBASE Hinted 相对不回收把 RSS 降低 **36–58%**。Cluster 7 降幅最大（58%），因其高偏斜把工作集集中到更少的热对象上；DBench 最小（36%），因其键区间局部性把访问分散得更均匀。把 OBASE 加到 TMO 上，相对仅 TMO 额外带来 **15–30%** 的节省，表明二者互补：TMO 识别可回收页，OBASE 确保这些页只含均匀冷数据。

**图 13. 冷阈值自适应（Meta CacheLib）。** 提升率（黑）与冷阈值 $C_t$（红）随时间变化。控制器自动调整 $C_t$ 以维持 1% 目标（紫）。

**自适应冷阈值（E4）。** 图 13 展示 OBASE 在 2.3 小时的 Meta CacheLib trace 中对负载动态的适应能力。启动时初始 $C_t = 3$ 导致过早降级，提升率飙至 14%。控制器每个扫描间隔递增 $C_t$；25 分钟内 $C_t$ 达到 18，提升率降到 1% 目标以下。收敛期间的高提升率**并不**表示性能退化：COLD 堆中的页在有后端显式换出前仍留在 DRAM，提升率反映的是负载行为而非后端决策（§3.4）。约 $t = 5400$ s 时负载发生偏移，先前冷的键变活跃，引起一次短暂尖峰；控制器提高 $C_t$，待新热集稳定后系统重新收敛。整个 trace 中 $C_t$ 在 10–20 个窗口间变化，同时把提升率维持在目标附近，说明 OBASE 能跟踪负载演进。

> **要点 5：OBASE 在特征各异的真实负载上带来一致的内存节省，并能动态适应不断变化的访问模式。**

---

## 6 相关工作

**对象级管理。** AIFM [57] 与 MIRA [35] 针对解耦远端内存在**对象粒度**上管理内存，绕过 OS 页管理以服务专门的用户态运行时；AIFM 构建于 Shenango 的绿色线程与内核旁路网络之上，用显式 `DerefScope` 守卫包裹远端内存访问。OBASE 与之互补：它作为**前端**为未修改的 OS **后端**重组地址空间，优化**分层什么**而非远程访问延迟，因此不做直接对比。`DerefScope` 的对比很有启发：它守卫每一次单独的远端内存解引用，以便运行时把对象调入；OBASE 的 TAG/ATC 作用域则包住整个公共操作，作为**静默性（quiescence）检查**，仅在没有线程活跃时才允许迁移——更接近基于 epoch 的回收，而非逐访问读屏障。

其他系统也通过指针间接达到不同目的：Midas [55] 为应用管理的**软状态**收集空闲内存、在压力下丢弃可重算数据；Atlas [26] 用混合的换页/运行时数据平面加速远端内存应用；二者都优化对**远端**内存的访问，而 OBASE 按观测访问强度重组**本地常驻**对象，使基于页的后端看到均匀冷热的页。Alaska [61] 用句柄式间接做堆压缩，但不跟踪对象热度、也不按访问强度组织内存。ObjecTier [53] 提出与 OBASE 同一高层愿景的非侵入式对象合并框架，但动机来自模拟，并把实现留作未来工作。

**分配时放置。** 静态放置方案 [27, 28, 40, 51, 66] 在分配时依据提示或剖析做决策。但对象热度既无法在分配时预知，也不随时间稳定（§2）。OBASE 在运行时跟踪访问模式，从而支持运行时迁移。

**页级分层。** TPP [49]、Memtis [44]、HawkEye [52]、TMO [62] 都工作在页粒度。即便缩小分层粒度的努力（如 Memtis 的动态 4KB 分类 [44]）也受页下限约束，因为不同热度的子页对象仍共享同一个 4KB 页（§2）。OBASE 在此下限之下工作——按对象粒度重组——并设计用于改进这些后端。

**垃圾回收与对象重定位。** GC 早已为重定位对象以改善局部性：分代收集器聚集新分配对象，剖析引导的重排序 [39] 在复制时把热对象分组，Bookmarking Collector [38] 在收集期间避免换页。OBASE 在重定位安全性上不同：ZGC [63] 与 Shenandoah [30] 用读屏障在线程持有过期指针时重定向访问；OBASE 采用受基于 epoch 的回收 [25, 31] 与 RCU [50] 启发的**基于静默性**的做法——只在对象的活跃线程计数归零时重定位，并通过 CAS 失败在任何并发访问下中止。目的也不同：GC 重定位是为了回收内存或改善缓存局部性，OBASE 重定位是为了提高**页利用率**，使分层机制看到均匀的页。

---

## 7 局限性与适用范围

OBASE 面向特定一类代码——非托管语言中的基于指针的并发数据结构——而非追求普遍适用。其需求限定了适用范围，我们明确列出以便用户判断契合度。

- **无指针稳定性。** 重定位会使跨操作缓存的裸指针失效（例如 Abseil map 或 STL 迭代器保留的地址）。调用方必须通过 guide 重新解析，正如指针不稳定容器本已要求的那样 [10]。
- **单一所有权。** guide 像 `std::unique_ptr` 一样主张独占所有权，迁移只更新该 guide。多个 guide 别名同一对象（共享图结构或双向链接节点）不受支持。CrestDB 通过插入时深拷贝来保证这一点。
- **无指针算术。** guide 不支持跨对象边界的 `+`/`[]`，因此连续数组、矩阵与打包列式数据在 OBASE 中不受支持；校验 pass 拒绝此类用法。
- **语言支持。** 设计依赖解引用运算符重载，Go 与 Java 不支持。
- **标注。** 开发者需标注少量指针字段，采用并非完全透明；其余由编译器完成（§4.7）。
- **硬件可组合性。** guide 编码复用高位地址位，若 Intel LAM [2]、ARM TBI [9] 或 HWASAN [18] 占用相同位则可能冲突；更宽的寻址 [5] 可缓解。我们面向非托管语言，因为多数延迟敏感的机房代码是 C++ 或 Rust；这些机制可推广到托管运行时。

---

## 8 结论

OBASE 表明：当应用与 OS **合作**而非绕过 OS 时，内存分层效果更好。通过重组虚拟地址空间使页边界与对象温度对齐，OBASE 让既有后端无需修改就更有效。方法有局限：需要开发者标注可迁移指针，且仅适用于基于指针的结构。然而，地址空间工程的原则超越分层本身：同样的技术可用于改进分代垃圾回收（按访问强度而非年龄分组）、减少跨 NUMA 节点的页伪共享（按访问模式分离对象）、增强隔离（按信任级别分组）。应用的地址空间布局本身就是通往 OS 的一条信道 [20]：通过把温度相近的对象分组，OBASE 把布局转化为一个对象级信号，使未修改的、基于页的分层机制能够据此行动，而两者之间无需任何新接口。

---

## 致谢

感谢 David Culler、Abhinav Sharma、Lilian Tsai、Qian Ge、Teresa Johnson、Sujay Yadalam，SystemsResearch@Google 与 ADvanced Systems Laboratory（ADSL）的同事，匿名评审与 shepherd 的宝贵反馈。本工作由 Google 的捐赠支持。

---

## 参考文献

（保留原文编号，供正文引用对应）

1. 2020. CXL And Gen-Z Iron Out A Coherent Interconnect Strategy.
2. 2021. Enable Intel LAM in Linux.
3. 2022. Compute Express Link.
4. 2022. Intel Optane DC PMM.
5. 2022. Road to 128-bit Linux.
6. 2022. Samsung Memory-semantic SSD.
7. 2024. Advanced profiling topics: PEBS and LBR.
8. 2024. DAMON: Data Access Monitor.
9. 2024. Top-Byte-Ignore (TBI) ARM.
10. 2025. Abseil Pointer Instability.
11. 2025. Azure delivers cloud VM with Intel Xeon 6 and CXL memory.
12. 2025. DynamoRIO: Dynamic Binary Instrumentation Framework.
13. 2025. Google Workload Traces Version 2.
14. 2025. LLVM-dev RFC: Sanitizer-based Heap Profiler.
15. 2025. Overview of Package util.concurrent.
16. 2025. Tagged Pointers.
17. 2025. Linux VM Kernel Doc.
18. 2026. Hardware-Assisted AddressSanitizer Design Documentation.
19. Appuswamy et al. 2019. The five-minute rule 30 years later and its impact on the storage hierarchy. CACM.
20. Arpaci-Dusseau & Arpaci-Dusseau. 2023. *Operating Systems: Three Easy Pieces* (1.10 ed.).
21. Banakar et al. 2025. Tidying Up the Address Space. DIMES '25.
22. Berg et al. 2020. The CacheLib Caching Engine. OSDI '20.
23. Blackburn et al. 2004. Myths and realities: the performance impact of garbage collection.
24. Cao et al. 2020. Characterizing, Modeling, and Benchmarking RocksDB Key-Value Workloads at Facebook. FAST '20.
25. Chandramouli et al. 2018. FASTER: A Concurrent Key-Value Store with In-Place Updates. SIGMOD '18.
26. Chen et al. 2024. A Tale of Two Paths: Toward a Hybrid Data Plane for Efficient Far-Memory Applications. OSDI '24.
27. Chen et al. 2020. ATMem: adaptive data placement in graph applications. CGO '20.
28. Dulloor et al. 2016. Data tiering in heterogeneous memory systems. EuroSys '16.
29. Duraisamy et al. 2023. Towards an Adaptable Systems Architecture for Memory Tiering at Warehouse-Scale. ASPLOS '23.
30. Flood et al. 2016. Shenandoah. PPPJ '16.
31. Fraser. 2003. Practical lock-freedom.
32. Gray & Graefe. 1997. The five-minute rule ten years later.
33. Gray & Putzolu. 1987. The 5 minute rule.
34. Guerraoui & Trigonakis. 2016. Optimistic concurrency with OPTIK.
35. Guo et al. 2023. Mira: A Program-Behavior-Guided Far Memory System. SOSP '23.
36. Harris. 2001. A Pragmatic Implementation of Non-blocking Linked-Lists. DISC '01.
37. Herlihy et al. 2007. A simple optimistic skiplist algorithm. SIROCCO '07.
38. Hertz et al. 2005. Garbage collection without paging. PLDI '05.
39. Huang et al. 2004. The garbage collection advantage: improving program locality.
40. Johnson et al. 2021. RFC: IR metadata format for MemProf.
41. Kanev et al. 2015. Profiling a warehouse-scale computer.
42. Koh et al. 2019. Faster than Flash. IISWC '19.
43. Lagar-Cavilla et al. 2019. Software-Defined Far Memory in Warehouse-Scale Computers. ASPLOS '19.
44. Lee et al. 2023. MEMTIS. SOSP '23.
45. Leis et al. 2013. The adaptive radix tree. ICDE '13.
46. Li et al. 2023. Pond: CXL-Based Memory Pooling Systems for Cloud Platforms. ASPLOS '23.
47. Mao et al. 2012. Cache craftiness for fast multicore key-value storage. EuroSys '12.
48. Al Maruf & Chowdhury. 2020. Effectively Prefetching Remote Memory with Leap. ATC '20.
49. Al Maruf et al. 2022. TPP: Transparent Page Placement for CXL-Enabled Tiered Memory.
50. McKenney & Slingwine. 1998. Read-copy update.
51. Miucin & Fedorova. 2018. Data-driven spatial locality. MEMSYS '18.
52. Panwar et al. 2019. HawkEye. ASPLOS '19.
53. Pawar et al. 2025. ObjecTier: Non-Invasively Boosting Memory Tiering Performance. ICPE '25.
54. Pugh. 1990. Concurrent maintenance of skip lists.
55. Qiao et al. 2024. Harvesting Idle Memory for Application-managed Soft State with Midas. NSDI '24.
56. Raybuck et al. 2021. HeMem. SOSP '21.
57. Ruan et al. 2020. AIFM: High-Performance, Application-Integrated Far Memory. OSDI '20.
58. Sun et al. 2023. Demystifying CXL Memory with Genuine CXL-Ready Systems and Devices. MICRO '23.
59. Tirmazi et al. 2020. Borg: the next generation. EuroSys '20.
60. van Riel & Chegu. 2014. Automatic NUMA balancing.
61. Wanninger et al. 2024. Getting a Handle on Unmanaged Memory. ASPLOS '24.
62. Weiner et al. 2022. TMO: Transparent Memory Offloading in Datacenters. ASPLOS '22.
63. Yang & Wrigstad. 2022. Deep Dive into ZGC. ACM TOPLAS.
64. Yang et al. 2020. An Empirical Guide to the Behavior and Use of Scalable Persistent Memory. FAST '20.
65. Yang et al. 2020. A large scale analysis of hundreds of in-memory cache clusters at Twitter. OSDI '20.
66. Zhou et al. 2024. Characterizing a Memory Allocator at Warehouse Scale. ASPLOS '24.
