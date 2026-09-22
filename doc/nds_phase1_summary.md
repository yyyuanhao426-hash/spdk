# NDS Phase 1 阶段总结（SPDK 层中转路线验证）

> 时间：2026-09-14 ~ 2026-09-22 ｜ 状态：**Phase 1 收官，全链路首测通过**
> 配套文档：[nds_design.md](nds_design.md)（三层架构与 Phase 2 方案）、
> [agents/nds_company_validation_task.md](agents/nds_company_validation_task.md)（全过程指令/回执存档）

## 1. 背景与目标

NDS（NPU Direct Storage）：让昇腾 NPU 的 HBM 显存绕过 CPU 内存、经 URMA
总线（灵衢 UB）直连远端 NVMe 盘，复刻 GPU 版 GDS。分两个 Phase：

- **Phase 1（本总结范围）**：验证 SPDK 层中转路线（npu-staged）——NPU HBM
  影子缓冲 → host 暂存 → URMA → 远端 NVMe，不依赖内核改动
- Phase 2（后续）：直连路线（-M npu，peer-memory 直连），需内核 NPU 桥接模块

Phase 1 的验收目标（任务书第 0 节）：① 确认环境；② 编译通过；③ 跑通测试；
④ 采集硬件验证项数据。**四项全部达成。**

## 2. 测试拓扑演进（为什么是单机回环）

| 阶段 | 拓扑 | 结论 |
|------|------|------|
| 第一代 | 197(NPU 950PR) + 151(store V100) | URMA 链路可通，卡上层问题；197 被收回 |
| 第二代 | 133(NPU 950DT) + 245(store) | 确认两台**无 UB 总线连接**，通信本不可能，放弃 |
| 第三代 | **133 单机回环**（最终方案） | 本机 nvmf_tgt 模拟存储端（AIO 文件模拟盘），URMA 流量走本机两个 udma 设备 |

单机回环的意义：在公司**不存在**与 133 UB 互联的存储节点（已确认）的约束下，
用一台机器完成 SPDK URMA 传输层 + NPU 中转路线的**功能验证**。性能数据
（回环 + 模拟盘）不代表真实水平，"远端"语义留待真实拓扑。

## 3. 代码交付

### 3.1 spdk-urma（nds_v1 分支，Phase 1 主战场）

| 内容 | 说明 |
|------|------|
| urma_perf 新增 `-M npu` / `-M npu-staged` | NPU 内存 provider（dlopen AscendCL，零编译依赖）、设备无关抽象、预检/清理分流 |
| urma_register_seg_dmabuf dlsym 化 | 同一二进制兼容标准版与 gds 版 liburma |
| NPU 初始化前置 spdk_env_init | 规避 DPDK EAL 对 CANN 的干扰 |
| 协作文档体系 | 入职必读（测试/开发两份）+ 环境 SOP + 主对话文件（指令/回执全程存档） |
| **本次会话修复** | aclrtCreateContext 函数指针 typedef 补 deviceId 参数（CANN 2 参签名，原 1 参调用致 107001） |

### 3.2 umdk（gds 用户态库，TLV 协议适配，6 个提交）

gds 版 liburma（基于老版本 UMDK）与 133 运行内核（新版驱动）存在**能力清单
（query device attr，TLV 编码）的多重代差**，逐层修复后全部自然对齐、零
临时补丁。最终修复：**删除多余的 RESERVED out-type**（真根因）、删除多余
的 PORT_CNT 枚举项、port_cnt 数据指针路径修正。

### 3.3 urma_driver（内核驱动，存档备用）

ubase ctrlq 白名单缺 TPID_DESTROY_DONE（opcode 0x28）致 udma probe 全失败的
修复（0728ee4，本地存档未推送）。本环境未用到，供未来驱动换装场景。

## 4. 关键问题复盘（现象 → 根因 → 修复）

按"洋葱"顺序，每层修复揭开下一层：

| # | 现象 | 根因 | 修复 |
|---|------|------|------|
| 1 | 133↔245 URMA 建连全失败 | **两机无 UB 总线连接**（管理面/驱动均正常） | 拓扑放弃（此前误判为驱动版本问题，已纠正） |
| 2 | 同机两 URMA 进程 init 双双失败 4096 | gds liburma 的 provider 插件目录缺失 | 补齐 lib/urma/ 目录 |
| 3 | query device 报 Invalid attr（type 156/159/160/163） | gds 与运行内核的 TLV **宽度代差**（内核多字段加宽） | 按内核 spec 宽度逐项适配（临时变量中转法） |
| 4 | query 通过但**值全错**（ceq_cnt=0 → jfc 被拒） | gds 枚举**多一个 RESERVED 项**，其后字段语义整体错位一格 | 删 RESERVED（配合删 PORT_CNT），全部自然对齐 |
| 5 | create jfc 失败 | 语义错位的下游表象（ceq_cnt 误读为 0） | 随 #4 消失 |
| 6 | **import jetty 失败（get tp list -ENOENT）** | 同为语义错位下游表象（**"管理面阻断"假设被证伪**） | 随 #4 消失 |
| 7 | npu-staged 报 aclError 107001 | SPDK 侧 aclrtCreateContext typedef 少 deviceId 参数 | 补 2 参签名（6a5028e） |
| 8 | 历史：LOC_ACCESS_ERR / 507033 | 均为上述错位/环境问题的下游表象 | 随 #4/#7 **关闭，未再复现** |

**方法论沉淀**（本次验证最有效的三件武器）：
1. **最小复现程序 + sysfs 真值逐字段对照**——把"怪象"变成精确的字段级差异
2. **反汇编运行内核模块提取 TLV spec 表**（fill_spec 立即数 + append_type
   位域）——拿到协议真值，终结逐项试错
3. **分批指令制 + 原样回执**——测试 Agent 只做指令内操作，根因链条全程可溯

## 5. 最终验证数据（2026-09-22，133 单机回环，AIO 模拟盘）

| 组 | 参数 | errors | 带宽 / IOPS | 延迟 p50 / p99 (μs) |
|----|------|--------|-------------|---------------------|
| CPU 基线 | -M cpu -T 4 | 0 | 1.39 MiB/s / 355 | 304ms / 965ms |
| npu-staged 复现（卡 0） | -T 4 | 0 | 6.72 / 1720 | 10.9ms / 647ms |
| 换卡（卡 1） | -g 1 | 0 | 9.82 / 2514 | 10.6ms / 438ms |
| 单线程 | -T 1 | 0 | 0.49 / 125 | 124ms / 132ms |
| 8 线程 | -T 8 | 0 | 6.40 / 1638 | 10.8ms / 1.59ms |
| 1 分钟长跑 | -t 60 | 0 | 34.55 / 8844 | 2.9ms / 125ms |
| **64K 大 IO** | -o 65536 | 0 | **907 MiB/s / 14515** | 2.5ms / 121ms |

- 7 组全零错误，dmesg 无异常；换卡一次通过；长跑 53 万次 I/O 无退化
- **两个历史悬案关闭**：LOC_ACCESS_ERR、aclError 507033 未再复现
- NPU 初始化、HBM 影子缓冲、aclrtMemcpy 分级拷贝、URMA 传输、NVMe 落盘
  全链路功能正确

## 6. 遗留与 Phase 2 展望

1. **真实远端拓扑**：单机回环验证功能，性能与"远端"语义需 UB 互联的存储
   节点（公司当前没有）或平台侧管理面支持
2. **直连路线（Phase 2 核心）**：`-M npu` 的 peer-memory 注册已按预期收集
   到失败样例。需开发 `udma_npu_bridge.ko`（内核 NPU 桥接，pin HBM 物理页
   → SG 表）、CANN dmabuf 导出验证、gpu_p2p 契约泛化——方案见 nds_design.md
3. **gds liburma 适配产出**应回馈 UMDK 源头（TLV 对齐补丁已就绪，按需推送）
4. 共享机环境风险（外部清理/重启/时钟变更）已通过隐蔽工作目录 + 全程留档
   缓解，真实测试环境建立后此风险消除

## 7. 协作与执行统计

- 指令/批次：#1 ~ #33（含 4 轮换环境、14 个验证/修复批次）
- 参与方：用户（协调）、开发 Agent（代码/指令/根因分析）、测试 Agent（公司
  内网执行/取证）
- 全程原始输出存档于主对话文件第 9/10 节及各机 l*_log/ 目录
