# NDS Phase 1 阶段总结：我们实现了一个什么系统

> 时间：2026-09-14 ~ 2026-09-22 ｜ 状态：**Phase 1 收官，全链路首测通过**
> 配套：[nds_design.md](nds_design.md)（三层架构与 Phase 2 方案）、
> [agents/nds_company_validation_task.md](agents/nds_company_validation_task.md)（全过程存档）

## 1. 一句话：我们实现了什么

**一套跑在昇腾 NPU 上的、与 GPU 版 GDS 对齐的直接存储验证系统**：以 SPDK
`urma_perf` 为载体，实现了 NPU 内存路线的可插拔框架——NPU HBM 中的数据可以
经 host 中转（npu-staged，已验证通过）或经 peer-memory 直连（npu，接口已就
位、等待 Phase 2 内核桥接）两条路线，穿越 URMA 总线写入 NVMe 存储。

Phase 1 验收结论：**中转路线（npu-staged）端到端功能验证通过**——7 组参数
矩阵全零错误，64K 大 IO 吞吐 907 MiB/s，两个历史悬案（LOC_ACCESS_ERR、
507033）随根因修复一并关闭。

## 2. 设计上实现了哪几块能力

### 2.1 NPU 内存路线框架（urma_perf 的核心扩展）

在 SPDK 内存域模型之上实现了**设备无关的多路线抽象**：

```
urma_perf -M <route>
  ├─ cpu / posix          host 内存（原有路线，回归基线）
  ├─ npu-staged  ────────►  Phase 1 已验证：HBM 影子缓冲
  │                          ⇓ aclrtMemcpy 分级拷贝（DtoH/HtoD）
  │                          host 暂存缓冲 → URMA → NVMe
  └─ npu         ────────►  Phase 2：HBM 整块注册，peer-memory 直连
                             （接口就位，等内核 NPU 桥接模块）
```

设计要点：
- **NPU memory provider**（`urma_perf_npu.c`）：dlopen AscendCL 动态加载
  （编译期零依赖 CANN，无 CANN 机器上其他路线不受影响）、HBM 影子缓冲
  注册表、设备/上下文生命周期管理（init/set_device/create_context/
  destroy/reset 全链）
- **分级拷贝引擎**：每次 I/O 的 DtoH/HtoD 拷贝被计量（staged_copy 统计），
  路线行为可观测
- **扩展性**：新增一条内存路线只需实现 provider 接口 + 一个 `-M` 枚举，
  与 GPU 版 GDS 的 peermem/dmabuf 路线结构对齐，便于后续互相移植

### 2.2 双版本 liburma 兼容设计

同一份 urma_perf 二进制，经 dlsym 运行时解析 `urma_register_seg_dmabuf`
等 gds 扩展符号，可同时在两种环境运行：
- **标准版 liburma**（系统自带）→ host-only 路线（cpu/posix/npu-staged）
- **gds 版 liburma**（含 is_gpu_seg/直连注册扩展）→ 全路线含直连

这层解耦让"中转验证"不依赖任何内核改动，也让直连路线的启用与否变成纯粹的
库选择问题。

### 2.3 单机回环验证床

在公司不存在 UB 互联存储节点的约束下，设计并实现了**一台机器内的完整
NDS 拓扑**：

```
┌─────────────────── 133（NPU 机）───────────────────┐
│  urma_perf(-M npu-staged)      nvmf_tgt             │
│  NPU HBM→host→udmaA ══UB══> udmaB → URMA target    │
│                                → AIO bdev(模拟盘)   │
└────────────────────────────────────────────────────┘
```

nvmf_tgt 以 URMA transport 导出 AIO 文件模拟的 NVMe 命名空间——除物理介质
外，协议栈与真实远端 NVMe 完全一致。这使 NDS 的功能验证不再受制于硬件协调。

### 2.4 跨版本驱动适配层（gds liburma ⇄ 新版内核）

发现并修复了 gds 用户态库与运行内核驱动之间的**能力清单协议（TLV）代差**：
gds 基于老版本 UMDK，其 out-type 枚举与新版内核的 spec 表存在一项多余
（RESERVED）与多项宽度差异。最终交付一个**零临时补丁的适配层**——修正后
50 条 spec 逐项自然吻合。该适配层使 gds 库可与新版内核驱动协同工作，是
Phase 2 直连路线的前置条件。

## 3. 设计验证结论（2026-09-22，133 单机回环，AIO 模拟盘）

| 验证维度 | 结果 |
|----------|------|
| 路线功能 | npu-staged 端到端通过：HBM → 分级拷贝 → URMA → NVMe，预检 WRITE+READ 逐字节一致 |
| 路线区分度 | CPU 路线通过、npu 直连按预期失败——两条 NPU 路线行为分明，无假阳性 |
| 硬件无关性 | 卡 0 / 卡 1 均通过；8×950DT 的卡间无绑定 |
| 并发扩展 | 1/4/8 线程全部通过，errors=0 |
| 稳定性 | 1 分钟长跑 53 万次 I/O 零错误、零退化，dmesg 无异常 |
| 吞吐上限 | 64K 大 IO **907 MiB/s / 14515 IOPS**（回环+模拟盘拓扑） |
| 历史问题 | LOC_ACCESS_ERR、507033（aclrtCreateContext/HDC）**随根因修复关闭，未再复现** |

注：回环 + 模拟盘拓扑下数据为**功能正确性**证据，吞吐/延迟不代表真实
远端 NVMe 水平。

## 4. 设计边界（Phase 1 不含什么）

1. **直连**：`-M npu` 走 peer-memory 直连注册，需 Phase 2 的内核 NPU 桥接
   模块（`udma_npu_bridge.ko`）——已按预期采集到失败样例，作为 Phase 2 输入
2. **真实远端**：需与 NPU 机 UB 互联的存储节点（公司当前没有）或平台侧
   管理面支持
3. **性能结论**：回环拓扑下所有吞吐/延迟数据仅证明功能，不构成性能基线

## 5. Phase 2 入场条件（已备齐）

- 三层架构方案与契约设计：`nds_design.md`（NPU 桥接模块、契约泛化、
  CANN dmabuf 路线）
- `-M npu` 直连失败样例已采集（Phase 2 的第一份输入）
- gds liburma ⇄ 新版内核的适配层已就位（2.4）
- 验证床（单机回环）可复用：Phase 2 的桥接模块上线后，直连路线可在同一
  验证床上先行验证，无需等真实拓扑

## 6. 执行统计

- 指令/批次 #1~#33，4 轮环境，15 个验证批次；全程原始输出存档
- 协作：用户（协调）/ 开发 Agent（设计与代码）/ 测试 Agent（内网执行取证）
