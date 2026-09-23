# NDS Phase 2 阶段总结：直连路线的机制探索与验证

> 时间：2026-09-22 ~ 2026-09-23 ｜ 状态：**机制链验证 100% 完成；实施被环境与签名约束定局阻断**
> 配套：[nds_phase1_summary.md](nds_phase1_summary.md)（中转路线，已交付）、
> [nds_design.md](nds_design.md)（三层架构）、
> [agents/nds_company_validation_task.md](agents/nds_company_validation_task.md)（P2-0~P2-13 全程存档）

## 1. Phase 2 的目标

Phase 1 已交付中转路线（npu-staged）：NPU HBM 数据经 host 暂存后走 URMA
到盘。Phase 2 的目标是**直连路线**（`-M npu`）：HBM 整块注册给 URMA 设备
做 DMA，省掉中转拷贝——这是 NDS 对标 GPU 直连（GDS zero-copy）的终极形态。

核心命题：**在 133（标准 openEuler SP4 内核 + CANN 9.1.0，借用+保密约束）
上，能否让"HBM 显存"被 URMA 设备直接 DMA？**

## 2. 结论先行

**命题的技术部分已完全回答：该平台的 HBM 直连存在一条华为官方的机制链
（CASM/libhcomm/设备 kernel），且我们已在 133 上把这条链的每一环都实证
打通至设备 kernel 启动。但链的最后一环（AICPU 运行环境装载）被两道非
技术门槛阻断：设备侧 CMS 签名校验（拒自建包）+ 借用机的保密约束（禁系统
级写入）。**

即：**机制可行已证，实施需正式环境**（自有/授权机器 + 官方签名包）。

## 3. 探索历程：五次认知迭代（每次否决都换来更深的平台理解）

| 轮次 | 尝试 | 结果 | 获得的平台认知 |
|------|------|------|----------------|
| ① 自研内核桥接 | 参照仓内 NVIDIA 桥接模式（xingtong 的 gpu_p2p 框架）写 `udma_npu_bridge.ko` | ❌ 133 运行 udma.ko 未编译该框架（ENABLE=0），且其源码为华为内部版本、公开仓对 133 内核头编译不过 | 运行内核的 ub 驱动源不可得；桥接模块代码已备存 |
| ② non_pin 直注 | HBM device VA 直接 `urma_register_seg(non_pin=1)` | ❌ 注册成功但不可被 URMA jetty 远端 DMA（跨设备 LOC_ACCESS_ERR/同设备无 CQE） | **HBM device VA 是进程内 SVM 语义，不是 fabric 级地址**——这是华为架构的设计，不是缺陷 |
| ③ memfabric 发现 | 华为官方开源的同栈 HBM 直接访问库（vLLM-ascend 后端） | 🌟 机制链入口找到 | libhcomm/CASM 是华为的 HBM 共享官方机制 |
| ④ libhcomm 实测 | HcommMemReg 对 HBM 注册 | ✅ **成功**，且全程只走标准接口（davinci_manager VA→PA + URMA_CMD + ummu） | 控制面（Endpoint/注册/导出/导入/通道）host 侧全可用 |
| ⑤ 设备 kernel 数据面 | memfabric 的 AICPU kernel（HybmBatchRead/Write）+ aclrtLaunchKernelWithConfig | ⚠️ 启动链全通（launch=0），执行被 AICPU 侧 hcomm 实现缺失阻断；免安装路径侦察确认不存在；系统级安装又遇设备 CMS 签名校验 | **数据搬运 = 设备侧 AICPU kernel 承载**（华为架构决定）；设备包装载 = TSD/ini + CMS 签名校验（拒自建包） |

## 4. 平台机制认知（Phase 2 的核心技术产出）

以下均为实证结论（探针/strace/反汇编/双进程实验），对后续任何
HBM 直接访问类工作都是前置知识：

1. **HBM device VA 是进程内 SVM 语义**：同一 buffer 在不同进程的 device
   VA 相同（全局 SVM 空间），但 CPU/ACL 常规通路不可访问（SIGSEGV/107000），
   也不能作为 URMA jetty 的远端 DMA 地址
2. **HBM 跨进程/跨机共享的唯一官方机制 = CASM**（davinci_manager 上的
   CREATE_KEY/QUERY_SRC/DESTROY_KEY ioctl 族）：注册产出共享 key，
   MemExport 出 262B memDesc，对端 HcommMemImport 导入
3. **数据搬运由设备侧 AICPU kernel 承载**：host 侧 hcomm 读写接口是
   weak 占位；真实搬运 = AICPU kernel（HybmBatchRead/Write）经
   aclrtLaunchKernelWithConfig 启动执行
4. **AICPU kernel 装载 = TSD 包机制 + CMS 签名校验**：只认系统 ini 里
   的包（相对系统 install_path），且要求华为/社区签名——自建包被拒
5. **平台 URMA jetty 必须 CTP**（RTP 在内核 get_tp_list 即失败）
6. **memfabric swap 分配的 MEM_PAGE_HUGE flag 在本平台不支持**
   （0xFFFE NOT_SUPPORT，与大页数量无关）——华为自己的代码也有平台适配
   问题，修法 = 环境变量逃生门（已验证）或源码改 flag

## 5. 已交付资产

| 资产 | 位置 | 状态 |
|------|------|------|
| `udma_npu_bridge.ko`（NPU 桥接模块，参照仓内 NVIDIA 桥接模式） | urma_driver npu_bridge/（24a1014） | 编译通过，待 gpu_p2p 框架可用的内核 |
| memfabric 修复验证（MEM_PAGE_HUGE 逃生门） | env + 源码定位 | 已在 133 验证（两示例端到端通过） |
| hcomm 控制面完整用法（Endpoint/MemReg/Export/Import/Channel，EID 取 hccl_rootinfo.json） | 探针源码 + 全套 strace 取证 | 已实证 |
| AICPU kernel 交叉编译与启动链 | p211/p213_log | 已实证至 launch=0 |
| 平台机制认知（本文件第 4 节） | 任务书 P2 系列批次存档 | 全程可溯 |

## 6. 阻塞链与解除条件

```
HBM 直连实施
  ← 需 AICPU 侧 hcomm 实现装入 CANN OPP 树（系统级安装）
     ← 需华为/社区签名的 memfabric/HYBM 包（设备 CMS 校验拒自建包）
        ← 需向华为/memfabric 社区获取官方签名 release
     ← 且系统级写入触碰借用机保密约束
        ← 解除 = 正式授权环境（自有/正式借用机器）
```

**解除条件的优先序建议**：① 申请正式测试环境（NDS 产品化绕不开，所有
Phase 2 产出直接迁移）；② 向 memfabric 社区/华为获取签名包并在 133 上
完成最后一步（需权衡保密约束）；③ 若平台团队可提供 GDR 支持的驱动构建，
udma_npu_bridge.ko 路线同步复活。

## 7. 执行统计

- 批次 P2-0 ~ P2-13（14 批），其中 5 批触发方向修正，全部根因闭环
- 涉及源码：memfabric_hybrid（华为开源）、urma_driver、umdk、openEuler
  OLK-6.6 内核树——交叉验证
- 方法论延续 Phase 1：最小复现 + 字段级对照 + 反汇编取证 + 双进程实验
