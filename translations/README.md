# 论文索引

## 总表

| # | 论文 | 出处 | 一句话 | 总结 | 完整翻译 | PDF |
|---|------|------|--------|------|----------|-----|
| 1 | **xTier** Intelligent Tiering for CXL | arXiv '26 | eBPF + PEBS + 34 µs MLP 进内核，14/18 最快，迁移量少 13–22× | [总结](xTier-2026-arXiv-Intelligent-Tiering-for-CXL-Enabled-Memory-总结.md) | — | [PDF](../papers/xTier-2026-arXiv-Intelligent-Tiering-for-CXL-Enabled-Memory.pdf) |
| 2 | **FlexMem** Adaptive Page Profiling and Migration | ATC '24 | 双剖析器（PEBS+NUMA fault）+ 按提升失败率自适应降级量，比 Tiering-0.8/TPP/MEMTIS 平均快 32%/23%/27% | [总结](Xu-2024-ATC-FlexMem-Adaptive-Page-Profiling-and-Migration-总结.md) | — | [PDF](../papers/Xu-2024-ATC-FlexMem-Adaptive-Page-Profiling-and-Migration.pdf) |
| 3 | **ArtMem** RL-Enabled Adaptive Migration | ISCA '25 | 内核内 Q-learning 调迁移量与热度阈值，奖励为快层访问占比，比 7 个 SOTA 平均快 114% | [总结](Yi-2025-ISCA-ArtMem-RL-Adaptive-Tiered-Memory-总结.md) | — | [PDF](../papers/Yi-2025-ISCA-ArtMem-RL-Adaptive-Tiered-Memory.pdf) |
| 4 | **FarSight** DL-Driven Prefetching for Far Memory | arXiv '25 | 序数词表(K=64)+future map 解耦语义与地址，3K 参数 RetNet 藏在缺页 I/O 中推理，比 FastSwap 最多快 3.6× | [总结](Huang-2025-arXiv-FarSight-DL-Prefetching-Far-Memory-总结.md) | — | [PDF](../papers/Huang-2025-arXiv-FarSight-DL-Prefetching-Far-Memory.pdf) |

## 相关资源

- xTier 代码：[`a85tract/xtier`](https://github.com/a85tract/xtier)（即本仓 `code/xtier` submodule）
