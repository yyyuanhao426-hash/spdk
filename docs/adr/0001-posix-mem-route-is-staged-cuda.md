# posix 内存路线改为 HBM→host 分级路线，原行为改名 cpu

urma_perf 最初的 `-M posix` 直接用 spdk_dma_zmalloc 的 host 内存注册 URMA，完全不碰
CUDA——但对比实验真正要测的"无 GDR 路径"是 **HBM 产出数据 → 每 I/O 拷到 host 暂存
缓冲 → NIC DMA 发送**。2026-09-09 决定：`-M posix` 语义改为这条分级路线（cuMemAlloc
影子缓冲 + 4K 对齐 host 暂存缓冲，仅暂存缓冲注册；拷贝耗时单独打印、不计入 NVMe 延迟），
原行为保留但改名 `-M cpu`。名字跟着测的路径走：posix 在对比语境里就是"标准无 GDR 基线"，
旧语义数据尚未用于结论，全部重跑。

## Considered Options

- 新语义用新名字（如 `-M staged`）、posix 保持旧义 —— 被否：以后每次引用"posix 基线"
  都要解释它其实没有 GPU 参与，域语言持续分裂。
- 只加新模式、不动 posix —— 同上，且四个模式名更混乱。

## Consequences

- 改版前用 `-M posix` 收集的数据与改版后**不可比**，一律重跑。
- 纯 CPU 基线（如无 GPU 机器上的冒烟测试）用 `-M cpu`。
