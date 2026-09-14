# [RFC] Add URMA (Unified Remote Memory Access) transport for NVMe/NVMf

> 
> v1.2：补充架构总览、连接建立时序与 RDMA↔URMA 概念映射；实现定位保持纯提案。
> 代码后续按社区流程走 Gerrit（`refs/for/master`），GitHub issue 仅用于引入讨论。

# Sighting report

本 issue 跟踪为 SPDK 引入 URMA（Unified Remote Memory Access）transport 的工作项。
URMA 是新型的智算网络互联协议，具备：

- **零 HOST staging**：数据路径不经主机内存中转，端到端直达；
- **全新对象模型**：JFC（Jetty File Context）/ JFR（Jetty File Region）、EID、bonding 等概念，与 RDMA 的 QP/CQ 模型完全不同；
- **自有建链流程**：不经过 rdma_cm，无法塞进现有 RDMA transport。
URMA 的统一编程抽象与用户态库由 UMDK 提供：[https://atomgit.com/openeuler/umdk](https://atomgit.com/openeuler/umdk)
因此本提案选择**新增独立 transport**（`URMA`），而非扩展 RDMA transport：URMA 的对象模型与 RDMA 的 QP/CQ 模型无对应关系，强行塞入现有 RDMA 代码路径会在数据面引入无谓的抽象开销，且使两种协议的状态机互相纠缠。

## 架构总览

host 侧与 target 侧各自实现为独立 transport，共享同一份线上协议定义与设备抽象；两侧均以 UMDK 用户态库（`liburma`）为唯一 URMA 依赖：

```
flowchart TD
    subgraph host[Host 侧（initiator）—— lib/nvme]
        A1[nvme.c<br/>trtype 解析与 transport 分发] --> A2[nvme_urma.c<br/>ctrlr 构造 / qpair 连接<br/>IO 提交与完成 / poll group]
        A2 --> A3[nvme_urma_common.c<br/>环境变量选项<br/>内存注册 Provider<br/>HOST/CUDA/ROCm/NPU/XDS]
    end
    subgraph shared[共享协议层]
        B1[nvme_urma_internal.h<br/>capsule 命令/响应<br/>endpoint 与数据描述符<br/>设备抽象]
    end
    subgraph target[Target 侧 —— lib/nvmf]
        C1[nvmf transport 框架] --> C2[urma.c<br/>transport 创建/监听<br/>accept 分阶段握手<br/>capsule 收发与数据投递<br/>poll group]
    end
    L[liburma / UMDK]
    A2 -.->|引用| B1
    C2 -.->|引用| B1
    A2 --> L
    A3 --> L
    C2 --> L
```

### 概念映射

为便于评审对照，URMA 对象与 RDMA 概念的对应关系如下（实现中保留 URMA 原生命名，不做 RDMA 术语重命名）：

| RDMA 概念 | URMA 对应 | 说明 |
| --- | --- | --- |
| QP | Jetty | 数据收发队列 |
| CQ | JFC | 完成队列 |
| MR | URMA 内存注册段 | host 与加速器内存统一注册 |
| GID | EID | 端点标识 |
| rdma_cm 建链 | socket 控制连接 + hello 交换 | 自有建链流程 |

### 连接建立时序

```
sequenceDiagram
    participant H as Host（nvme_urma.c）
    participant T as Target（urma.c）
    H->>T: connect_socket() 建立 socket 控制连接
    Note over T: accept() 接受连接后逐阶段执行
    T->>T: device_open() 打开 URMA 设备
    T->>T: get_socket_addresses() 取两端地址
    T->>T: create_jetty() 创建 target 侧 Jetty
    H->>H: create_jetty() 创建 host 侧 Jetty
    H->>T: exchange_hello() 交换 endpoint 描述符
    T->>T: handshake() 校验并绑定
    Note over H,T: 失败时逐阶段输出错误日志并释放已建资源
    T->>T: new_qpair() 进入 NVMf 建连流程
```

为完整落地该 transport，需要实现以下条目：

- 1.
- 2.
- 3.
- 4.
- 5.
- 6.
- 7.
- 8.
- 9.
- 10.
- 
  11. 

## Expected Behavior

- `trtype: URMA` 端到端可用：host 侧（`lib/nvme/nvme_urma.c`）与 target 侧（`lib/nvmf/urma.c`）完整 transport；
- 现有 transport 与默认构建零影响（`CONFIG_URMA=n` 默认关闭，全部新代码条件编译）。

## Current Behavior

- SPDK 无法识别 `trtype: URMA`（fabrics trtype 中不存在该值）；
- 无 URMA initiator/target 实现，URMA 网卡在 SPDK 生态中不可用。

## Possible Solution

实现上述条目 1–11 即可提供完整的 NVMe/NVMf URMA transport 支持。非 URMA 路径零额外分支（trtype 解析 O(1) 查表 + 常量对齐），新代码集中于独立文件，无跨模块耦合。

## Steps to Reproduce

（不适用——新特性提案，无复现步骤。）

## Context (Environment including OS version, SPDK version, etc.)

- SPDK 基线：`3230130e6a`（master）
- OS：Linux（URMA 内核驱动 + UMDK 用户态库已部署）
- 硬件：URMA 网卡（具体型号待补：____）
- UMDK 版本：待补：____

## Non-Goals

以下明确不在本提案范围（多路径重提交时的数据一致性保障依赖路径级 quiesce 语义，须单独设计后再议）：

- 重连与超时恢复
- 多路径
- target 侧 P2P / XDS（后续阶段）