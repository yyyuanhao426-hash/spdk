# 从零理解 SPDK NVMe over URMA：本次代码修改说明

## 1. 这篇文档解决什么问题

本文面向第一次接触 SPDK、NVMe-oF、RDMA 和 URMA 的读者，解释当前分支为了实现 NVMe over URMA 做了什么修改，以及一条 I/O 请求从 GPU buffer 到远端 SSD 的完整过程。

本文所说的 **NoF** 指 **NVMe over Fabrics（NVMe-oF）**。NVMe-oF 是更常见、更正式的写法，后文统一使用 NVMe-oF。

读完本文后，应当能够回答以下问题：

1. NVMe、NVMe-oF、RDMA、URMA 和 GDR 分别是什么。
2. 为什么 URMA 不能简单伪装成 NVMe/RDMA。
3. SPDK initiator 和 SPDK NVMf target 分别做什么。
4. GPU 到 SSD 的 WRITE，以及 SSD 到 GPU 的 READ，分别经过哪些步骤。
5. 为什么保留 SPDK 原有 NVMe/NVMf 状态机，同时新增 URMA 请求状态。
6. 为什么异构内存注册需要单独抽象，而不能只调用原来的 HOST 内存注册接口。
7. 当前代码已经实现什么，还有什么没有实现。

本文描述的是当前实验分支中的实现，不代表已经发布的 NVMe/URMA 行业标准。

---

## 2. 先认识整个系统

### 2.1 SSD 和 NVMe

SSD 是存储设备，NVMe 是主机与 SSD 交流的一套协议。

应用程序不会直接向 SSD 说“把这个文件写进去”，而是通过操作系统或用户态存储框架生成 NVMe command。常见命令包括：

- NVMe WRITE：把主机内存中的数据写入 SSD。
- NVMe READ：把 SSD 中的数据读入主机内存。
- IDENTIFY：查询 controller 或 namespace 信息。
- GET/SET FEATURES：读取或设置 controller 属性。

一条 NVMe I/O 通常包含：

- opcode：READ 或 WRITE 等操作类型。
- namespace ID：访问哪个 namespace。
- LBA：从 SSD 的哪个逻辑块开始。
- length：访问多少逻辑块。
- data pointer：数据位于哪块内存。
- command ID：用来匹配异步 completion。

传统 NVMe SSD 一般通过 PCIe 连接到本机。命令和数据都在同一台服务器内部流动。

### 2.2 NVMe-oF

NVMe over Fabrics 把 NVMe 协议扩展到网络上，使一台机器可以访问另一台机器上的 NVMe SSD。

NVMe-oF 中有两个重要角色：

- **Initiator**：发起 NVMe 请求的一方，可以理解为存储客户端。
- **Target**：接收请求并操作真实 SSD 的一方，可以理解为存储服务端。

最简单的结构如下：

~~~text
Initiator                                     Target

应用程序                                      SPDK NVMf target
   |                                                |
NVMe command  ----------- 网络传输 -------------> NVMe command
数据 buffer   <---------- 网络传输 -------------> target buffer
                                                    |
                                                 bdev 层
                                                    |
                                                 NVMe SSD
~~~

NVMe-oF 本身主要规定“远端 NVMe 的语义”。真正如何在网络上传输 command、data 和 completion，由具体 transport binding 决定。

现有常见 transport 包括：

- NVMe/TCP：通过 TCP 传输。
- NVMe/RDMA：通过 RDMA verbs 及相应协议传输。
- NVMe/FC：通过 Fibre Channel 传输。

本次修改增加的是新的实验性 transport：

- NVMe/URMA：使用 URMA 搬运 NVMe payload。

### 2.3 SPDK 是什么

SPDK 是一个面向高性能存储的用户态框架。它尽量避免传统内核 I/O 路径中的中断、上下文切换和多余内存复制。

本文涉及 SPDK 的三部分：

#### SPDK NVMe initiator

它负责生成 NVMe command、管理 NVMe controller 和 qpair，并将请求交给某个 transport。

例如，应用调用 SPDK NVMe READ 后，公共 NVMe 层会把请求交给 TCP、RDMA、PCIe 或本文新增的 URMA transport。

#### SPDK NVMf target

它负责接收远端 initiator 的 NVMe command，验证 controller、namespace 和请求状态，然后调用后端 bdev。

#### SPDK bdev

bdev 是统一的块设备抽象。target 不需要知道后端一定是真实 SSD，也可以是 malloc bdev、AIO、RAID 或其他块设备。

因此，一条远端 WRITE 的后半段大致是：

~~~text
NVMf transport
      |
SPDK NVMf controller/request 状态机
      |
SPDK bdev
      |
NVMe driver
      |
SSD
~~~

本次实现保留了这条成熟路径。

### 2.4 Qpair 是什么

NVMe 使用 submission queue 提交 command，使用 completion queue返回结果。两者合起来通常称为 queue pair，简称 qpair。

NVMe-oF 通常至少有：

- 一个 admin qpair：用于 CONNECT、IDENTIFY 和 property 操作等管理请求。
- 一个或多个 I/O qpair：用于 READ、WRITE 等数据请求。

qpair 使多个请求可以并行在途，并通过 command ID 匹配 completion。

---

## 3. RDMA、URMA 和 GDR 分别是什么

### 3.1 RDMA

RDMA 的核心能力是：网卡可以直接访问已注册的内存，并在远端 CPU 不复制 payload 的情况下执行 READ 或 WRITE。

典型 RDMA 软件接口是 libibverbs。应用需要创建和管理：

- device/context；
- protection domain；
- memory region；
- queue pair；
- completion queue；
- remote key 和 address。

SPDK 已经存在 `nvme_rdma.c` 和 `nvmf/rdma.c`，它们实现的是 NVMe/RDMA transport binding。

### 3.2 URMA

URMA 是与 RDMA 平行的一套远程内存访问接口。它有自己的设备、endpoint、内存和 completion 对象，例如：

- EID：endpoint identifier。
- Jetty：发送和接收任务的 endpoint/queue 对象。
- JFS/JFR：发送端和接收端资源。
- JFC：completion queue。
- target segment：已注册或导入的内存 segment。

URMA 能提供远端 READ/WRITE，但它的对象模型、API、控制面和资源生命周期不是 libibverbs。

因此，本次设计把 URMA 当作一个新的 SPDK transport，而不是在 NVMe/RDMA 源码中增加若干 `if (urma)`。

### 3.3 为什么不能复用 NVMe/RDMA transport

可以复用的是高层设计经验，例如：

- 一个 qpair 需要发送 command 并轮询 completion。
- WRITE 前要让 target 获得 initiator 数据。
- READ 完成后要把 target 数据送回 initiator。
- 请求结束时要释放注册和导入资源。

不能直接复用的是：

- RDMA verbs 对象和调用。
- RDMA CM 建链流程。
- RDMA QP/CQ/MR 数据结构。
- NVMe/RDMA transport-specific wire binding。
- RDMA rkey、QP number 等地址描述。

如果强行把 URMA 塞进 NVMe/RDMA，会产生两个问题：

1. URMA 的实现长期依赖 RDMA 内部结构，无法作为独立社区 transport 演进。
2. 任何 RDMA 状态机改动都可能破坏 URMA，URMA 特有能力也难以表达。

当前代码因此新增独立的 `SPDK_NVME_TRANSPORT_URMA`、独立 initiator 文件和独立 target 文件。

### 3.4 GDR

GDR 一般指 GPU Direct RDMA，即网卡直接访问 GPU buffer，不先把 payload 复制到 HOST buffer。

没有 GDR 时，GPU WRITE 可能是：

~~~text
GPU buffer -> HOST bounce buffer -> NIC -> 网络
~~~

有 GDR 时，希望变成：

~~~text
GPU buffer -> NIC -> 网络
~~~

本文第一阶段消除的是 **initiator 侧 GPU 与 HOST 之间的 bounce copy**。

需要特别注意：当前 target 仍然使用 SPDK NVMf 分配的 HOST buffer，再由 bdev 把数据写入 SSD。因此当前完整 WRITE 路径是：

~~~text
Initiator GPU buffer
        |
        | URMA READ，initiator 侧无 HOST staging
        v
Target HOST buffer
        |
        | SPDK bdev/NVMe DMA
        v
SSD
~~~

这不是“SSD 直接 DMA 到远端 GPU”的最终形态。target 侧存储 P2P/XDS 是后续阶段。

---

## 4. 本次修改的总体目标

本次修改希望建立下面这条可运行的实验路径：

~~~text
SPDK urma_perf 或其他 SPDK 应用
        |
        | NVMe READ/WRITE
        v
SPDK NVMe/URMA initiator transport
        |
        | command/control：TCP bootstrap socket
        | payload：URMA READ/WRITE
        v
SPDK NVMf/URMA target transport
        |
        | spdk_nvmf_request_exec()
        v
SPDK 原有 NVMf controller + bdev 状态机
        |
        v
NVMe SSD
~~~

设计约束包括：

1. URMA 与 NVMe/RDMA 源码相互独立。
2. 不修改 Mooncake 和 UMDK；测试程序直接放在 SPDK 中。
3. HOST、GPU、NPU 和后续 XDS buffer 使用同一套可扩展注册接口。
4. 保留 SPDK 原有 NVMe controller、NVMf request 和 bdev 语义。
5. 在 SPDK `urma_perf` 中提供可以直接连接 SPDK target 的端到端测试模式。

---

## 5. 修改前和修改后的区别

### 5.1 修改前

SPDK 能识别 PCIe、TCP、RDMA、VFIO-user 等 transport，但不认识 `URMA`。

即使 UMDK 能单独执行 URMA READ/WRITE，SPDK 也不知道：

- 如何通过 URMA 建立 NVMe qpair；
- 如何把 NVMe command 交给远端 target；
- 如何用 URMA 搬运 NVMe payload；
- 什么时候才能返回 NVMe completion；
- 如何注册 GPU/NPU/XDS buffer。

### 5.2 修改后

应用可以用 `trtype:URMA` 选择新的 transport。

SPDK 增加了：

- `SPDK_NVME_TRANSPORT_URMA` transport type；
- `--with-urma` 编译开关；
- NVMe/URMA initiator；
- NVMf/URMA target；
- URMA runtime、device、JFC 和内存注册公共层；
- 异构内存 provider API；
- URMA transport 的基础 parser 单元测试。

SPDK `urma_perf` 增加了：

- CUDA GPU buffer 分配和注册；
- NVMe/URMA READ/WRITE 数据正确性检查；
- 可配置 I/O size、线程数、batch size 和测试时间；
- 带宽、IOPS、平均时延和 p50/p99/p99.9 统计；
- DMA-BUF 与 peer-memory 注册路径统计。

---

## 6. SPDK 中具体修改了什么

### 6.1 编译和功能开关

修改文件：

- `CONFIG`
- `configure`
- `lib/nvme/Makefile`
- `lib/nvmf/Makefile`

新增配置：

~~~text
CONFIG_URMA
CONFIG_URMA_DIR
~~~

编译示例：

~~~bash
./configure --with-urma=/path/to/UMDK_tool_netlab
make -j
~~~

`configure` 会检查：

- `urma_api.h`；
- `urma_ubagg.h`；
- `liburma`。

关闭 URMA：

~~~bash
./configure --without-urma
~~~

关闭时不会编译 URMA transport，也不会引入 `liburma`。

### 6.2 新的 transport type

修改文件：

- `include/spdk/nvme.h`
- `lib/nvme/nvme.c`

新增枚举：

~~~c
SPDK_NVME_TRANSPORT_URMA = 4098
~~~

并增加字符串转换：

~~~text
"URMA" <-> SPDK_NVME_TRANSPORT_URMA
~~~

使用大于 8 bit 的实验值，是因为 NVMe 规范目前没有为 URMA 分配正式的 8-bit transport type。该数值是分支内部值，未来社区标准化时可能变化。

### 6.3 URMA 公共层

新增文件：

- `lib/nvme/nvme_urma_internal.h`
- `lib/nvme/nvme_urma_common.c`

公共层负责 initiator 和 target 都会用到的能力：

1. 引用计数方式初始化和反初始化 URMA runtime。
2. 接受 `URMA_EEXIST`，避免进程中已有 URMA runtime 时误报失败。
3. 枚举 URMA device 和 EID。
4. 创建 URMA context。
5. 自动选择 active port，或使用指定 port。
6. 按 Mooncake 默认值创建 JFC。
7. 设置 bonding/multipath 模式。
8. 创建 SPDK memory domain。
9. 注册 HOST 或 accelerator memory。
10. 在请求结束时注销 segment 并释放 provider pin。

### 6.4 NVMe/URMA initiator

新增文件：

- `lib/nvme/nvme_urma.c`

initiator transport 负责：

- 创建 controller 和 qpair；
- 为每个 qpair 创建 URMA context、JFC 和 Jetty；
- 与 target 交换 EID 和 Jetty ID；
- 提交 NVMe command capsule；
- 注册 initiator data buffer；
- 等待 target 返回 NVMe completion；
- 根据 command ID 找到原请求并调用 SPDK completion callback。

每个 qpair 维护一个 outstanding request 队列。请求提交后不会立即结束，必须等 target 的 completion capsule 返回。

### 6.5 NVMf/URMA target

新增文件：

- `lib/nvmf/urma.c`

target transport 负责：

- 创建 URMA listener；
- 接受 initiator 连接；
- 交换 endpoint 描述；
- 为 qpair 创建 request pool；
- 接收 NVMe command capsule；
- 根据 READ/WRITE 方向执行 URMA 数据搬运；
- 调用原有 `spdk_nvmf_request_exec()`；
- 在 bdev 和 URMA 都完成后返回 NVMe completion。

target 没有重新实现 namespace、controller、NVMe command 校验或 SSD driver。它只负责 transport 特有的连接、数据搬运和 completion 时序。

### 6.6 符号导出和基础测试

修改文件：

- `lib/nvme/spdk_nvme.map`
- `test/unit/lib/nvme/nvme.c/nvme_ut.c`

增加了 URMA API 符号导出，以及 `URMA` transport 字符串解析测试。

---

## 7. 控制面和数据面如何划分

### 7.1 什么是控制面

控制面负责建立连接、交换资源标识和传递 command/completion。它关注“要做什么”。

### 7.2 什么是数据面

数据面负责搬运真正的 payload。它关注“数据从哪里移动到哪里”。

### 7.3 当前第一版的实现

当前代码使用：

- TCP：bootstrap、endpoint 描述、NVMe command capsule、NVMe completion capsule。
- URMA：有 payload 的 NVMe READ/WRITE 数据搬运。

也就是说，它不是把现有 NVMe/TCP transport 拿来复用，而是新的 NVMe/URMA transport 暂时使用一个 TCP socket 承担控制消息。

~~~text
TCP 控制通道：
HELLO / HELLO_RSP / NVMe CMD / NVMe CPL

URMA 数据通道：
GPU/HOST/NPU payload READ / WRITE
~~~

这样设计是为了先验证以下最关键问题：

1. SPDK 是否能把 URMA 作为独立 transport 接入。
2. NVMe request 与 URMA completion 的时序是否正确。
3. GPU buffer 是否能通过 URMA 直接访问。
4. target 是否能继续使用原有 NVMf/bdev 状态机。

未来可以把 command capsule 也放到 URMA SEND/RECV 或正式标准定义的控制通道中，但那需要先确定协议、credit、重连和 discovery 格式。

---

## 8. 建立连接时发生什么

### 8.1 Target 初始化

target 创建 `URMA` transport 时：

1. 读取默认配置和环境变量。
2. 初始化 URMA runtime。
3. 找到指定 URMA device，未指定时选第一个可用设备。
4. 找到 EID。
5. 创建 URMA context。
6. 设置 bonding 模式。
7. 选择 active port。
8. 创建 JFC。
9. 创建 TCP listener，等待 initiator。

### 8.2 Initiator 创建 controller

initiator 使用 `trtype:URMA` 连接时：

1. SPDK 根据字符串找到 `urma_ops`。
2. 构造 NVMe controller。
3. 创建 admin qpair。
4. 为 qpair 打开 URMA device/context。
5. 创建 JFC 和 Jetty。
6. 连接 target 的 bootstrap socket。

### 8.3 Endpoint HELLO

双方交换以下信息：

- wire magic 和版本；
- qpair ID；
- EID；
- Jetty ID；
- transport mode；
- 最大 queue depth；
- 最大 I/O size。

收到远端信息后，双方调用 `urma_import_jetty()`。RC 模式下还会调用 `urma_bind_jetty()`。

双方会取本地与远端能力的较小值，避免 initiator 提交 target 无法处理的 queue depth 或 I/O size。

### 8.4 NVMe Fabrics CONNECT

URMA endpoint 建好后，并不代表 NVMe controller 已经连接完成。

initiator 还要提交标准 NVMe Fabrics CONNECT command。target 的公共 NVMf 层会：

- 检查 host NQN 和 subsystem NQN；
- 创建或关联 controller；
- 建立 admin 或 I/O qpair；
- 返回 controller ID 和 NVMe completion。

这一步复用了 SPDK 原有 `nvme_fabric_qpair_connect_async()`、`nvme_fabric_qpair_connect_poll()` 和 NVMf controller 逻辑。

---

## 9. NVMe WRITE：GPU 数据如何写入 SSD

假设 initiator 的 GPU buffer 中有一页 4 KiB 数据，要写入远端 SSD。

### 9.1 Initiator 提交请求

应用提交 NVMe WRITE 后：

1. SPDK 公共 NVMe 层创建 `nvme_request`。
2. 请求被交给 `nvme_urma_qpair_submit_request()`。
3. transport 判断 payload 是否是当前支持的连续 buffer。
4. 根据 SPDK memory domain 判断它是 HOST、CUDA、ROCm、NPU 或 XDS buffer。
5. 调用公共内存注册层。
6. 将 NVMe command、URMA segment、地址和长度装入 command capsule。
7. 通过控制 socket 把 command capsule 发给 target。
8. 请求进入 outstanding 队列。

此时 initiator 不会返回 completion，因为数据还没有进入 SSD。

### 9.2 Target 拉取 GPU 数据

target 收到 WRITE command 后：

1. 从 request pool 取一个请求对象。
2. 调用 SPDK NVMf buffer 接口分配 target HOST buffer。
3. 导入 initiator 发来的 remote segment。
4. 注册 target HOST buffer 为 local segment。
5. 提交 URMA READ：

~~~text
源：Initiator GPU buffer
目标：Target HOST buffer
~~~

这里使用 URMA READ，是因为 target 主动从 initiator 拉取 WRITE payload。

### 9.3 URMA 完成后执行 SSD WRITE

target 轮询 JFC。只有收到成功 completion 后，才调用：

~~~c
spdk_nvmf_request_exec(&request);
~~~

随后请求进入 SPDK 原有 NVMf/bdev 路径，最终写入 NVMe SSD。

### 9.4 SSD 完成后返回 NVMe completion

bdev WRITE 完成后：

1. NVMf 公共层调用 URMA transport 的 `req_complete()`。
2. target 填写 NVMe completion status、command ID 和 SQ head。
3. target 通过控制 socket返回 completion capsule。
4. 释放 target HOST buffer。
5. 注销 local segment。
6. unimport initiator remote segment。

initiator 收到 completion 后：

1. 根据 command ID 查找 outstanding request。
2. 注销 GPU segment。
3. 释放 provider pin。
4. 调用应用的 SPDK completion callback。

完整时序如下：

~~~text
Initiator                      Target                         SSD
    |                             |                            |
    | NVMe WRITE command          |                            |
    |---------------------------->|                            |
    |                             | 分配 HOST buffer           |
    |<====== URMA READ GPU =======|                            |
    |                             | URMA completion            |
    |                             |---------- bdev WRITE ----->|
    |                             |<--------- SSD completion ---|
    |<----- NVMe completion ------|                            |
    |                             |                            |
~~~

---

## 10. NVMe READ：SSD 数据如何进入 GPU

READ 与 WRITE 的方向相反。

### 10.1 Initiator 提交 READ

initiator 注册目标 GPU buffer，并发送 NVMe READ command capsule。此时 GPU buffer 只是数据最终要写入的位置。

### 10.2 Target 先读取 SSD

target：

1. 分配 HOST buffer。
2. 不立即执行 URMA 操作。
3. 调用 `spdk_nvmf_request_exec()`。
4. bdev 从 SSD 读取数据到 target HOST buffer。

### 10.3 SSD READ 完成后推送到 GPU

bdev READ 成功后，target 提交 URMA WRITE：

~~~text
源：Target HOST buffer
目标：Initiator GPU buffer
~~~

这里使用 URMA WRITE，是因为 target 主动把 READ 结果推入 initiator buffer。

### 10.4 URMA 完成后才返回 NVMe completion

只有 JFC 表明 URMA WRITE 已完成，target 才发送 NVMe completion。

如果 target 在 URMA WRITE 前就返回 NVMe completion，应用可能开始读取尚未写完的 GPU buffer，这会产生数据错误。因此 NVMe completion 必须同时代表：

- SSD READ 已完成；
- 数据已到达 initiator GPU buffer。

完整时序：

~~~text
Initiator                      Target                         SSD
    |                             |                            |
    | NVMe READ command           |                            |
    |---------------------------->|                            |
    |                             |---------- bdev READ ------>|
    |                             |<--------- SSD completion ---|
    |<====== URMA WRITE GPU ======|                            |
    |                             | URMA completion            |
    |<----- NVMe completion ------|                            |
    |                             |                            |
~~~

---

## 11. 为什么 SPDK 原有状态机仍然可以使用

“增加 URMA transport”并不意味着所有 NVMe/NVMf 状态都要重新实现。

可以继续复用的状态包括：

- NVMe controller 初始化与销毁；
- admin qpair 和 I/O qpair 的 NVMe 语义；
- Fabrics CONNECT；
- subsystem 和 namespace 查找；
- NVMe command 校验；
- bdev READ/WRITE；
- SSD completion；
- NVMe status code。

这些状态与底层网络是 TCP、RDMA 还是 URMA 没有直接关系。

必须新增的是 transport 特有状态：

~~~text
FREE
  |
  v
NEED_BUFFER
  |
  +-- WRITE --> PULLING --URMA完成--> EXECUTING
  |
  +-- READ -----------------------> EXECUTING
                                      |
                       READ成功 ------+
                                      v
                                   PUSHING
                                      |
                                  URMA完成
                                      v
                              SEND_COMPLETION/FREE
~~~

这些状态回答的是：

- target buffer 是否准备好；
- WRITE payload 是否已经从 initiator 拉回；
- bdev 是否已经执行；
- READ payload 是否已经推入 initiator buffer；
- 是否可以安全返回 NVMe completion。

因此准确的说法是：

> SPDK 原有 NVMe/NVMf 状态机继续使用；URMA transport 只新增它必须负责的数据搬运子状态。

---

## 12. 为什么异构内存生命周期需要单独设计

### 12.1 HOST buffer 相对简单

HOST buffer 通常有普通 CPU 虚拟地址。URMA 可以 pin 对应页面并建立 segment，结束后 unregister。

### 12.2 GPU/NPU buffer 不等同于 HOST buffer

GPU 或 NPU 地址可能具有以下特点：

- CPU 不能直接访问。
- 不一定能通过普通 `pin_user_pages` 注册。
- 需要厂商 runtime 保证 allocation 在 I/O 期间不释放。
- 可能通过 DMA-BUF fd 对外共享。
- 可能依赖 peer-memory kernel module。
- 不同 accelerator 的 API 完全不同。

因此不能只保存一个 `void *`，然后把所有内存都当成 HOST 内存。

### 12.3 新增 memory provider API

新增公共头文件：

- `include/spdk/nvme_urma.h`

支持的内存类型：

~~~text
HOST
CUDA
ROCM
NPU
XDS
~~~

accelerator provider 提供三个主要动作：

- `pin()`：保持 allocation 有效，并准备设备访问。
- `export_dmabuf()`：可选，导出 DMA-BUF fd 和 offset。
- `unpin()`：请求结束后释放 provider 资源。

公共层负责：

1. 引用 provider，防止有请求时被卸载。
2. 调用 provider pin。
3. 优先尝试 `urma_register_seg_dmabuf()`。
4. 当前 UMDK DMA-BUF 尚不支持时，回退到 `is_gpu_seg` 注册路径。
5. 请求完成后 unregister segment。
6. 调用 provider unpin。
7. 减少 provider 引用计数。

这种结构使 SPDK 核心不需要直接包含 CUDA、ROCm 或 NPU SDK 头文件。

### 12.4 XDS 如何接入

XDS 后续可以注册自己的 provider：

~~~text
XDS allocation
    |
XDS provider.pin/export_dmabuf
    |
SPDK NVMe/URMA memory region
    |
URMA target segment
~~~

只要 provider 能保证 allocation 生命周期，并提供 DMA-BUF 或可被 URMA peer-memory 注册的地址，NVMe/URMA transport 不需要为 XDS 重写一套状态机。

---

## 13. SPDK GPU 到 SSD 测试程序

UMDK 源码不再修改。测试程序位于 SPDK 的
`examples/nvme/urma_perf/urma_perf.c`，直接使用 SPDK NVMe public API 连接
NVMf/URMA target。

程序从 CUDA Driver API 分配 GPU buffer，通过 `spdk_memory_domain` 将其标记为
CUDA memory，并向 NVMe/URMA 注册 CUDA provider。计时前会执行 GPU WRITE、SSD
READ 和逐字节比较；正式测试输出带宽、IOPS、平均时延及 p50/p99/p99.9。

I/O size、线程数、batch size、运行时间、GPU、namespace 和起始 LBA 均可配置。
完整编译与验证步骤见 [NVMe/URMA GPU 到远端 SSD 测试指南](nvme_urma_gpu_perf.md)。

---

## 14. 默认参数和开关

默认参数参考 Mooncake 当前 URMA 实现：

| 参数 | 默认值 | 含义 |
|---|---:|---|
| transport mode | RM | URMA 传输模式 |
| JFC count | 2 | 每个 device context 的 completion queue 数量 |
| JFC depth | 4096 | completion queue 深度 |
| Jetty count | 1 | 第一版每个 qpair 一个 Jetty |
| Jetty depth | 2048 | Jetty queue 深度 |
| priority | 15 | URMA 发送优先级 |
| max SGE | 5 | URMA Jetty 能力配置；NVMe MVP 当前只使用一个连续 SGE |
| RNR retry | 7 | receiver-not-ready 重试参数 |
| error timeout | 17 | URMA error timeout 参数 |
| token | `0xACFE` | 实验性固定 token |
| max I/O size | 128 KiB | 第一版单请求最大 payload |

SPDK 环境变量：

| 变量 | 作用 |
|---|---|
| `SPDK_URMA_TRANS_MODE` | `RM`、`RC` 或 `UM` |
| `SPDK_URMA_DEV_NAME` | 指定 URMA device |
| `SPDK_URMA_EID_INDEX` | 指定 EID index |
| `SPDK_URMA_ACTIVE_PORT` | 指定 active port；未设置则自动选择 |
| `SPDK_URMA_JFC_COUNT` | JFC 数量 |
| `SPDK_URMA_JFC_DEPTH` | JFC 深度 |
| `SPDK_URMA_JETTY_COUNT` | Jetty 数量配置；MVP 每个 qpair 实际创建一个 |
| `SPDK_URMA_JETTY_DEPTH` | Jetty 深度 |
| `SPDK_URMA_MAX_IO_SIZE` | initiator 最大 I/O 大小 |
| `SPDK_URMA_BONDING_BALANCE` | 开启 bonding balance |
| `SPDK_URMA_BONDING_MULTIPATH_ENABLE` | 开启 bonding multipath |

为了便于与 Mooncake 使用相同部署环境，以下 Mooncake 变量也可作为低优先级兼容项：

~~~text
MC_URMA_TRANS_MODE
MC_URMA_ACTIVE_PORT
MC_URMA_BONDING_BALANCE
MC_URMA_BONDING_MULTIPATH_ENABLE
~~~

如果 SPDK 和 Mooncake 变量同时存在，优先使用 `SPDK_URMA_*`。

perftest 还支持：

| 变量 | 默认值 |
|---|---|
| `PERFTEST_NVME_SUBNQN` | `nqn.2016-06.io.spdk:cnode1` |
| `PERFTEST_NVME_HOSTNQN` | perftest 自带 host NQN |
| `PERFTEST_NVME_NSID` | `1` |
| `PERFTEST_NVME_BLOCK_SIZE` | `4096` |
| `PERFTEST_NVME_SLBA` | `0` |

---

## 15. 最小运行示例

以下命令是联调示例，实际设备、bdev 和 subsystem 参数应根据环境调整。

### 15.1 Target 创建 URMA transport

先启动 SPDK NVMf target，并创建 bdev、subsystem 和 namespace，然后执行：

~~~bash
scripts/rpc.py nvmf_create_transport -t URMA
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 \
    -t URMA -a 0.0.0.0 -s 4420
~~~

### 15.2 测试 GPU Direct WRITE

~~~bash
build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:192.0.2.10 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' \
  -w write -o 4096 -T 4 -b 32 -t 30 -g 0 -n 1 -l 0
~~~

### 15.3 测试 GPU Direct READ

~~~bash
build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:192.0.2.10 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' \
  -w read -o 4096 -T 4 -b 32 -t 30 -g 0 -n 1 -l 0
~~~

程序正式计时前会自动执行 GPU WRITE、SSD READ 和逐字节比较，所以不再依赖
UMDK `urma_perftest`。完整步骤见 [测试指南](nvme_urma_gpu_perf.md)。

---

## 16. 应该如何验证这套实现

建议按层次验证，不要第一次就同时调 GPU、URMA、NVMf 和真实 SSD。

### 阶段 1：编译和 parser

目标：确认 URMA 开关打开和关闭时都能编译。

- `--without-urma` 构建不受影响。
- `--with-urma` 能找到 UMDK 头文件和库。
- URMA transport parser 单元测试通过。

### 阶段 2：HOST + malloc bdev

目标：排除 GPU 和真实 SSD 因素。

- 建立 admin qpair。
- 建立 I/O qpair。
- WRITE 成功。
- READ 成功。
- READ 回的数据与 WRITE pattern 相同。

### 阶段 3：HOST + NVMe bdev

目标：验证真实 SSD 后端。

- 检查 namespace 和 LBA。
- 执行 WRITE/READ 回读校验。
- 检查 SSD error log。

### 阶段 4：GPU peermem

目标：验证 `is_gpu_seg` 和 peer-memory 路径。

- 确认 GPU allocation 没有退化成 HOST。
- 确认 UMDK 将 segment 识别为 GPU segment。
- 检查请求期间没有 D2H/H2D staging。

### 阶段 5：GPU DMA-BUF

目标：验证真正的 DMA-BUF import。

- GPU runtime 成功导出 fd。
- `urma_query_dmabuf_caps()` 报告支持。
- `urma_register_seg_dmabuf()` 成功。
- UMDK/kernel driver 完成 dma-buf attach/map。
- 数据校验和长时间压力测试通过。

### 阶段 6：故障测试

至少覆盖：

- target 断开；
- URMA completion error；
- SSD I/O error；
- 非法 command length；
- 超过 max I/O size；
- qpair 销毁时仍有在途请求；
- GPU allocation 在错误路径中的释放。

---

## 17. 当前实现的限制

当前代码是端到端 MVP，不应直接视为生产版本。

主要限制如下：

1. **没有正式标准 binding。** URMA 尚无正式 NVMe-oF 8-bit TRTYPE 和 TSAS 定义。
2. **控制面暂用 TCP。** command 和 completion 还没有改为 URMA SEND/RECV。
3. **只支持连续 payload。** initiator 当前只接受一个连续 buffer，不支持多 SGL/IOV。
4. **没有注册缓存。** 每个有数据的请求都会注册和注销内存，性能会受影响。
5. **target 仍使用 HOST buffer。** 尚未实现 SSD 与 accelerator 的 target 侧 P2P/XDS 路径。
6. **DMA-BUF 依赖尚未完整落地。** 当前 UMDK 分支中的接口可能回退到 `is_gpu_seg`。
7. **控制 socket 部分操作仍为同步操作。** 慢连接或拥塞下需要进一步异步化。
8. **恢复能力不足。** 重连、timeout、keepalive 和完整 abort/flush 仍需补充。
9. **安全机制仅为实验配置。** 固定 token 不能作为生产环境的认证方案。
10. **尚未完成目标硬件编译和验证。** 当前开发环境只能执行静态检查。

---

## 18. 后续演进方向

建议后续按以下顺序推进：

1. 在目标 Linux 环境修到 SPDK 和 UMDK 全量编译通过。
2. 跑通 HOST + malloc bdev WRITE/READ 校验。
3. 跑通 HOST + 真实 NVMe SSD。
4. 跑通 GPU peermem，并证明没有 HOST staging。
5. 完成 UMDK/kernel DMA-BUF ABI，实现稳定 GDR。
6. 增加内存注册缓存和失效机制。
7. 增加多 qpair、多 SGL 和并发请求。
8. 将控制消息迁移到正式 URMA transport protocol。
9. 增加 discovery、错误恢复和协议兼容性测试。
10. 接入 Mooncake NoF segment；Mooncake 只负责选择 SPDK URMA transport 和传递 memory domain/provider 信息。

---

## 19. 代码导航

### SPDK

| 文件 | 作用 |
|---|---|
| `include/spdk/nvme.h` | 增加 URMA transport type |
| `include/spdk/nvme_urma.h` | HOST/GPU/NPU/XDS memory provider 公共接口 |
| `lib/nvme/nvme.c` | URMA transport 字符串解析 |
| `lib/nvme/nvme_urma_internal.h` | 默认参数、wire message 和内部数据结构 |
| `lib/nvme/nvme_urma_common.c` | URMA runtime、device、JFC、memory registration |
| `lib/nvme/nvme_urma.c` | NVMe/URMA initiator transport |
| `lib/nvmf/urma.c` | NVMf/URMA target transport |
| `CONFIG`、`configure` | `--with-urma` 构建开关 |
| `lib/nvme/Makefile` | 编译 initiator 和公共层 |
| `lib/nvmf/Makefile` | 编译 target transport |
| `test/unit/lib/nvme/nvme.c/nvme_ut.c` | transport parser 测试 |
| `examples/nvme/urma_perf/urma_perf.c` | CUDA 显存到远端 SSD 的正确性与性能测试 |
| `doc/nvme_urma_gpu_perf.md` | 编译、部署、运行和验收说明 |

UMDK 仅作为外部 `liburma` 依赖，本次不修改其文件。

---

## 20. 一句话总结

本次修改没有把 URMA 伪装成 RDMA，而是在 SPDK 中增加了一个独立 NVMe/URMA transport：它复用成熟的 NVMe controller、NVMf request 和 bdev 状态机，新增 URMA endpoint、异构内存注册以及 READ/WRITE 数据搬运状态，并通过 SPDK `urma_perf` 建立从 GPU buffer 到远端 SSD 的第一条可联调路径。
