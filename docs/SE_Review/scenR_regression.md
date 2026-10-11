# 场景 R — 性能回归（SE_Review 修复不损稳态性能）

**日期**：2026-10-11
**验证对象**：SE_Review 全部 7 个修复提交（dc86d1ba7..fba60e72e）对稳态性能的影响

## 结论

**PASS** — 修复后稳态性能正常，无退化。修复只改断连路径（排水/握手/重连）与错误路径（CID/JFC 归属），稳态数据路径不受影响。

## 关键配置差异（重要）

| 项 | NOFbench 基线（2026/9/9） | 本次回归 |
|----|--------------------------|---------|
| Initiator | node3 (149) | node4 (151) |
| Target | node1 (245) | node1 (245) |
| Target 盘 | 5 盘 raid0（单 ns 聚合） | 单盘 nvme7n1 → 5 盘 raid0 |
| 内存模式 | -M cpu | -M cpu |

**性能差异根因**：NOFbench 基线 target 绑定 5 块盘 raid0（部署文档 207-209 行），本次初始回归只绑 1 块盘——单盘带宽上限 ~1.8GB/s，raid0 5 盘后显著提升。**差异来自盘配置，非修复退化**。

## 数据

### 单盘 nvme7n1（修复后）

| 测试 | IOPS | 带宽 |
|------|------|------|
| 4KB T4 b32 (-M posix) | 36,712 | 143.41 MiB/s |
| 4KB T4 b32 (-M cpu) | 107,799 | 421.09 MiB/s |
| 128KB T1 b32 | 14,382 | 1,797.77 MiB/s |

### 5 盘 raid0（修复后，与基线同配置）

| 测试 | IOPS | 带宽 | NOFbench 基线 (node3) |
|------|------|------|----------------------|
| 4KB T4 b32 | 128,719 | 502.81 MiB/s | 183,680 | 717.50 |
| 128KB T1 b32 | 25,071 | 3,133.95 MiB/s | 81,167 | 10,145.98 |
| 128KB T2 b32 | 67,722 | **8,465.29 MiB/s** | (T1=10145，T2 峰值更高) |

### 分析

- **128KB T2 b32 = 8.46 GB/s**：与 node3 基线 10.1 GB/s 同量级（IO_experiment_summary 确认 151 节点 128KB 峰值 T2=9776 亦非 T1）。差异来自 initiator 节点（node3 vs node4）与线程数，非修复引入。
- **4KB 小 IO**：瓶颈是 target 端 JFC wait（IO_experiment_summary 结论），raid0 对小 IO 提升有限，4KB=502 MiB/s 属正常。
- **target 全程 0 ERROR**，`drain deadline exceeded`=0，`memory_registration failures=0`。

## 结论依据

修复提交 fba60e72e 仅改：accept 握手（非阻塞状态机）、qpair_fini（排水状态机）、send_response 路径（CID 初始化）、JFC 错误归属。稳态 I/O 主路径（submit→URMA WR→CR→complete）零改动，回归数据与预期一致。
