# 从 NVMe over RDMA 到 NVMe over URMA

## 1. 文档目的

本文以当前 workspace 中的 SPDK 源码为准，说明 NVMe over RDMA 从 transport 初始化、连接建立、I/O 提交、target 数据搬运到 request 释放的完整过程，并据此划分引入独立 NVMe/URMA transport 时需要复用、替换和新增的代码。

本文重点回答三个问题：

1. SPDK 的 NVMe/RDMA initiator 和 NVMe-oF/RDMA target 如何建立连接。
2. NVMe WRITE 和 NVMe READ 中，command、payload 和 completion 分别由谁发送。
3. URMA 与 RDMA 平行存在时，SPDK 哪些通用逻辑可以复用，哪些 RDMA 专属逻辑必须替换。

本文描述的源码版本为本 workspace 中 SPDK commit <code>3230130e6</code>。关键源码包括：

- <code>lib/nvme/nvme_rdma.c</code>：NVMe/RDMA initiator transport。
- <code>lib/nvmf/rdma.c</code>：NVMe-oF/RDMA target transport。
- <code>lib/nvme/nvme_transport.c</code>：initiator transport registry。
- <code>lib/nvmf/transport.c</code>：target transport registry。
- <code>include/spdk/nvme.h</code>：<code>spdk_nvme_transport_ops</code>。
- <code>include/spdk/nvmf_transport.h</code>：<code>spdk_nvmf_transport_ops</code>。

## 术语

- **Initiator**：提交 NVMe command 的 host 端；**target**：接收 command 并导出 NVMe namespace 的存储端。
- **Capsule**：transport 承载的 NVMe command 或 completion message。大块 I/O payload 通常独立传输。
- **Qpair**：NVMe submission/completion queue pair。SPDK transport 为每个 qpair 建立对应的传输资源。
- **RDMA CM（Connection Manager）**：负责地址解析、route resolution、connect、accept 和 disconnect event 的连接管理接口。
- **QP（Queue Pair）**：RDMA send queue 与 receive queue 的组合。
- **CQ（Completion Queue）**：保存 RDMA work completion 的队列；**SRQ（Shared Receive Queue）**：多个 QP 可共享的 receive queue。
- **WR（Work Request）**：提交给 RDMA device 的 SEND、RECV、READ 或 WRITE 操作；**WC（Work Completion）**：WR 完成后写入 CQ 的记录。
- **MR（Memory Region）**：注册给 RDMA device 的内存区域。<code>lkey</code> 授权本地 device 访问，<code>rkey</code> 授权 remote peer 执行 RDMA READ/WRITE。
- **SGL（Scatter-Gather List）**：描述一个或多个 payload range。NVMe/RDMA keyed SGL 携带 remote address、length 和 rkey。
- **HOST_TO_CONTROLLER**：payload 从 initiator 流向 target；**CONTROLLER_TO_HOST**：payload 从 target 流向 initiator。
- **VA（Virtual Address）**：进程虚拟地址；**NUMA（Non-Uniform Memory Access）**：SPDK 用于让 qpair、poller 和 memory 靠近对应 CPU/device 的拓扑信息。
- **TSAS（Transport Specific Address Subtype）**：NVMe-oF discovery entry 中描述 transport-specific address 的字段。
- **EID（Endpoint Identifier）**：URMA/UB endpoint 的标识符。
- **Jetty**：管理 URMA I/O task 和 received message 的 queue，可视为 URMA command 的执行端口。
- **JFS（Jetty for Send）**：提交 DMA task 或发送 message；**JFR（Jetty for Receive）**：准备 message receive resource；**JFC（Jetty for Completion）**：保存 JFS/JFR completion record。
- **UMDK**：本 workspace 提供 URMA 用户态 API、provider 和 control-plane 组件的开发套件。
- **UVS**：UMDK 的 URMA control-plane service，负责 endpoint 和 transport path 等资源的协商或管理。
- **XDS**：本文对后续 SSD-to-accelerator data path 及其 memory-provider contract 的代称。
- **GDR（GPU Direct RDMA）**：device 直接访问 GPU buffer 的数据路径。本文沿用 GDR 表示 URMA device 访问 GPU buffer 且 payload 不经过 HOST staging。

## 2. NVMe over RDMA 的核心模型

NVMe/RDMA 将 NVMe command capsule 和 NVMe completion capsule 作为 RDMA SEND/RECV message 传输。大块 payload 通常不放入 capsule，而由 target 根据 initiator 提供的 keyed SGL 发起 RDMA READ 或 RDMA WRITE。

~~~text
Initiator                                      Target

NVMe command + keyed SGL
       |                                           |
       |-------------- RDMA SEND ----------------->|
       |                                           |
       |       target 解析 SGL 中的 VA + rkey       |
       |                                           |
WRITE: |<------------- RDMA READ ------------------|
READ:  |<-------------- RDMA WRITE ----------------|
       |                                           |
       |<------------- RDMA SEND ------------------|
       |              NVMe completion              |
~~~

方向容易混淆。NVMe WRITE 表示数据从 initiator 写入 SSD，因此 target 通过 RDMA READ 从 initiator buffer 拉取数据。NVMe READ 表示数据从 SSD 读回 initiator，因此 target 通过 RDMA WRITE 把数据推入 initiator buffer。

小型 HOST_TO_CONTROLLER payload 可以使用 in-capsule data。此时 command 和 payload 一起通过 SEND 到达 target，无需额外 RDMA READ。

## 3. SPDK 如何挂接 RDMA transport

### 3.1 Initiator 注册 <code>spdk_nvme_transport_ops</code>

<code>lib/nvme/nvme_rdma.c</code> 定义 <code>rdma_ops</code>，并通过：

~~~c
SPDK_NVME_TRANSPORT_REGISTER(rdma, &rdma_ops);
~~~

注册到 initiator transport registry。主要回调包括：

| 回调 | 作用 |
|---|---|
| <code>ctrlr_construct</code> | 创建 RDMA controller transport object |
| <code>ctrlr_scan</code> | 复用通用 Fabrics controller scan |
| <code>ctrlr_create_io_qpair</code> | 创建 I/O qpair |
| <code>ctrlr_connect_qpair</code> | 通过 RDMA CM 建立 qpair |
| <code>ctrlr_get_memory_domains</code> | 暴露 RDMA memory domain |
| <code>qpair_submit_request</code> | 构造 keyed SGL 并发送 command |
| <code>qpair_process_completions</code> | 轮询 CQ，处理 SEND 和 RECV completion |
| <code>poll_group_*</code> | 管理共享 CQ 和多个 qpair |

Property Get/Set、Fabrics scan 等逻辑使用通用 NVMe Fabrics 实现，不在 RDMA transport 内重复实现。

### 3.2 Target 注册 <code>spdk_nvmf_transport_ops</code>

<code>lib/nvmf/rdma.c</code> 定义 <code>spdk_nvmf_transport_rdma</code>，并通过：

~~~c
SPDK_NVMF_TRANSPORT_REGISTER(rdma, &spdk_nvmf_transport_rdma);
~~~

注册到 NVMf target。主要回调包括：

| 回调 | 作用 |
|---|---|
| <code>create/destroy</code> | 创建和销毁 RDMA transport |
| <code>listen/stop_listen</code> | 创建 RDMA CM listener |
| <code>listener_discover</code> | 生成 RDMA discovery entry |
| <code>poll_group_create/add/poll</code> | 创建 CQ/SRQ/poller，并处理 completion |
| <code>req_get_buffers_done</code> | target buffer 异步分配完成后继续 request |
| <code>req_complete</code> | NVMf core/backend 完成后继续 transport 流程 |
| <code>req_free</code> | 释放 transport request 和 buffer |
| <code>qpair_fini/abort</code> | 断连和异常清理 |

RDMA transport 负责网络数据移动。通用 NVMf core 负责 NVMe command 语义、controller、namespace 和 backend bdev I/O。

## 4. Initiator 初始化与连接

### 4.1 Controller 创建

应用以 <code>trtype:RDMA</code> 调用 SPDK probe/connect 后，transport registry 选择 <code>rdma_ops</code>。<code>nvme_rdma_ctrlr_construct()</code> 执行：

1. 分配 <code>nvme_rdma_ctrlr</code>。
2. 保存 transport ID 和 controller options。
3. 枚举 RDMA device，并查询 <code>max_sge</code> 等能力。
4. 调用 <code>nvme_ctrlr_construct()</code> 初始化通用 NVMe controller。
5. 创建非阻塞 RDMA CM event channel。
6. 创建 admin qpair。

admin qpair 先建立 transport connection，再发送标准 NVMe Fabrics CONNECT。controller 初始化随后通过 admin qpair 执行 Property Get/Set、Identify 等操作。

### 4.2 Qpair 建立

<code>nvme_rdma_ctrlr_connect_qpair()</code> 从 <code>traddr</code> 和 <code>trsvcid</code> 解析目标地址，并执行：

~~~text
create rdma_cm_id
  -> resolve_addr
  -> RDMA_CM_EVENT_ADDR_RESOLVED
  -> 创建 CQ、QP 等 verbs resource
  -> resolve_route
  -> RDMA_CM_EVENT_ROUTE_RESOLVED
  -> rdma_connect(private_data)
  -> RDMA_CM_EVENT_ESTABLISHED
~~~

RDMA CM private data 包含 qid、host receive queue size、host send queue size 和 controller ID。target 使用这些值与本地硬件限制共同计算 queue depth。

连接建立后，initiator：

1. 按 QP protection domain 创建 RDMA memory translation map。
2. 创建固定数量的 <code>spdk_nvme_rdma_req</code>。
3. 创建 response buffer 和 RECV WR。
4. 预投递 RECV，确保 target 能发送 NVMe completion。
5. 进入 <code>FABRIC_CONNECT_SEND</code>，发送 NVMe Fabrics CONNECT。
6. CONNECT completion 成功后将 qpair 转为 <code>RUNNING/CONNECTED</code>。

I/O qpair 重复相同 transport 连接过程，但 qid 非零，并在 controller 已存在的前提下关联到对应 NVMe controller。

## 5. Target 初始化与连接

### 5.1 Transport 创建

<code>nvmf_rdma_create()</code> 执行：

1. 解析 queue depth、max I/O、in-capsule size、CQ/SRQ 等配置。
2. 创建 RDMA CM event channel。
3. 创建 data WR mempool。
4. 枚举 RDMA device，并为每个 device 建立内部对象。
5. 为 CM event 和 IB async event 生成 poll fd。
6. 注册 <code>nvmf_rdma_accept</code> poller。

每个 SPDK NVMf poll group 再为可用 RDMA device 创建 poller。poller 持有 CQ，可选持有 SRQ，并管理分配到该 poll group 的 qpair。

### 5.2 Listener 与 connection request

<code>nvmf_rdma_listen()</code> 根据 <code>traddr/trsvcid</code>：

~~~text
getaddrinfo
  -> create rdma_cm_id
  -> bind_addr
  -> rdma_listen
~~~

<code>nvmf_rdma_accept()</code> 轮询 CM event channel。收到 <code>RDMA_CM_EVENT_CONNECT_REQUEST</code> 后，<code>nvmf_rdma_connect()</code>：

1. 校验 RDMA private data。
2. 根据 target 配置、local NIC、remote NIC 和 host queue size 计算 queue/read depth。
3. 分配 <code>spdk_nvmf_rdma_qpair</code>。
4. 保存 CM ID、device、qid 和 NUMA 信息。
5. 调用 <code>spdk_nvmf_tgt_new_qpair()</code> 把 qpair 交给 NVMf target。

NVMf core 为 qpair 选择 poll group。<code>nvmf_rdma_poll_group_add()</code> 随后：

1. 找到对应 RDMA device 的 poller。
2. 初始化 qpair request、receive、QP 等资源。
3. 把 qpair 加入 poller。
4. 调用 RDMA accept。

transport connection 建立后，target 仍需接收并执行标准 NVMe Fabrics CONNECT command。RDMA CM 只建立 transport qpair，NVMe Fabrics CONNECT 才建立 NVMe controller/qpair 语义。

## 6. Initiator 如何提交一个 request

### 6.1 分配 transport request 和 CID

<code>nvme_rdma_qpair_submit_request()</code> 从 qpair request pool 获取 <code>spdk_nvme_rdma_req</code>，并把该对象的 ID 写入 NVMe command CID。pool 暂时无空闲对象时返回 <code>-EAGAIN</code>。

### 6.2 将 buffer 转换为 RDMA SGL

<code>nvme_rdma_req_init()</code> 按 payload 类型选择构造方式：

- 无 payload：构造 null SGL。
- contiguous buffer：构造一个 keyed SGL。
- callback SGL 或 iovec：构造一个或多个 keyed SGL descriptor。
- 小型 HOST_TO_CONTROLLER payload：满足 in-capsule 条件时直接放入 capsule。

<code>nvme_rdma_get_memory_translation()</code> 有两条路径：

1. request 带 <code>spdk_memory_domain</code> 时，调用 <code>spdk_memory_domain_translate_data()</code>。
2. 普通 SPDK DMA buffer 使用 qpair 的 RDMA memory map 查询 MR。

translation 最终产生：

~~~text
local address
local key  (lkey)
remote key (rkey)
translated length
~~~

initiator 的 SEND WR 使用 lkey 读取本地 command/capsule。NVMe keyed SGL 将 address、length 和 rkey 交给 target，授权 target 对 payload buffer 执行 RDMA READ 或 RDMA WRITE。

### 6.3 发送 command

<code>_nvme_rdma_qpair_submit_request()</code>：

1. 将 request 加入 outstanding list。
2. 增加 qpair send depth。
3. 将 command SEND WR 放入 QP send queue。
4. 根据 batching 配置立即或稍后 flush doorbell。

initiator 在连接阶段已经预投递 response RECV，因此 command 发出后只需轮询 CQ。

## 7. Target 收到 command 后如何推进 request

### 7.1 RECV completion 创建 request

target poller 调用 <code>spdk_rdma_utils_poll_cq()</code>。收到 <code>IBV_WC_RECV</code> 后：

1. 找到 receive object 和 qpair。
2. 将 receive 放入 qpair incoming queue。
3. 从 free request pool 绑定一个 <code>spdk_nvmf_rdma_request</code>。
4. 将状态从 <code>FREE</code> 转为 <code>NEW</code>。
5. <code>nvmf_rdma_request_process()</code> 解析 command 和 transfer direction。

target RDMA request 的主要状态为：

~~~text
FREE
  -> NEW
  -> NEED_DATA_WR
  -> NEED_BUFFER
  -> HAVE_BUFFER
  -> DATA_TRANSFER_TO_CONTROLLER_PENDING
  -> TRANSFERRING_HOST_TO_CONTROLLER
  -> READY_TO_EXECUTE
  -> EXECUTING
  -> EXECUTED
  -> DATA_TRANSFER_TO_HOST_PENDING
  -> READY_TO_COMPLETE
  -> TRANSFERRING_CONTROLLER_TO_HOST / COMPLETING
  -> COMPLETED
  -> FREE
~~~

部分状态只在资源不足、多 SGL 或特定数据方向出现。

### 7.2 解析 SGL 并获取 target buffer

<code>nvmf_rdma_request_parse_sgl()</code> 校验：

- SGL type/subtype；
- keyed address 和 rkey；
- payload length；
- max I/O size；
- multi-SGL descriptor 数量。

随后调用 <code>spdk_nvmf_request_get_buffers()</code> 从通用 NVMf buffer pool 获取 target buffer，并把本地 iovec 填入 RDMA data WR。buffer 暂不可用时，request 留在 <code>NEED_BUFFER</code>，由 <code>req_get_buffers_done</code> 回调继续。

无 payload command 直接进入 <code>READY_TO_EXECUTE</code>。in-capsule WRITE 直接引用 receive capsule 内的数据，也不发起 RDMA READ。

## 8. NVMe WRITE 的完整路径

NVMe WRITE 的数据方向是 initiator 到 target/SSD。

~~~text
Initiator                                        Target

注册/查询 source buffer MR
构造 address + length + rkey
发送 NVMe WRITE capsule
       |                                             |
       |------------- RDMA SEND -------------------->|
       |                                             | NEW
       |                                             | parse keyed SGL
       |                                             | get target buffer
       |                                             | HAVE_BUFFER
       |                                             |
       |<------------ RDMA READ ---------------------|
       |                                             | TRANSFERRING_HOST_TO_CONTROLLER
       |                                             |
       |                                             | RDMA READ completion
       |                                             | READY_TO_EXECUTE
       |                                             | spdk_nvmf_request_exec()
       |                                             | bdev WRITE
       |                                             | req_complete()
       |                                             | EXECUTED
       |                                             |
       |<------------ NVMe completion SEND ----------|
       |                                             | COMPLETED -> FREE
应用 completion callback
~~~

关键顺序约束：

1. target 必须先完成 RDMA READ，才能调用 <code>spdk_nvmf_request_exec()</code>。
2. backend WRITE 完成后，NVMf core 调用 transport 的 <code>req_complete</code>。
3. target 再发送 NVMe completion。
4. target 在构造 response 前归还或重新 post command receive，保持 receive capacity。
5. response SEND completion 到达后，target 才释放 request 和 target buffer。

in-capsule WRITE 省略 RDMA READ，但仍需经过 NVMf core/backend 和 completion 发送。

## 9. NVMe READ 的完整路径

NVMe READ 的数据方向是 SSD/target 到 initiator。

~~~text
Initiator                                        Target

注册/查询 destination buffer MR
构造 address + length + rkey
发送 NVMe READ capsule
       |                                             |
       |------------- RDMA SEND -------------------->|
       |                                             | parse keyed SGL
       |                                             | get target buffer
       |                                             | READY_TO_EXECUTE
       |                                             | spdk_nvmf_request_exec()
       |                                             | bdev READ
       |                                             | req_complete()
       |                                             | EXECUTED
       |                                             |
       |<------------ RDMA WRITE --------------------|
       |<------------ NVMe completion SEND ----------|
       |                                             | COMPLETED -> FREE
应用 completion callback
~~~

<code>req_complete</code> 把 request 转为 <code>EXECUTED</code>。成功的 CONTROLLER_TO_HOST request 进入 pending RDMA WRITE queue。send depth 足够后，<code>request_transfer_out()</code> 投递 data WR 和 response WR。

当前 RDMA target 在同一有序 send queue 中提交 RDMA WRITE 和 response SEND。SEND completion 到达时，代码将 request 标记为 <code>COMPLETED</code>，并同时扣除关联 data WR 的 send depth。因此 successful NVMe completion 不会先于该 request 的 RDMA WRITE。

## 10. Initiator 如何判断 request 结束

initiator 为每个 request 分别记录：

- <code>NVME_RDMA_SEND_COMPLETED</code>：command SEND 的本地 completion。
- <code>NVME_RDMA_RECV_COMPLETED</code>：收到 target 返回的 NVMe completion。

只有两个 flag 都完成后，<code>nvme_rdma_request_ready()</code> 才调用通用 NVMe request completion。这样即使 target response 很快返回，initiator 也不会在本地 SEND WR 仍引用 request object 时提前复用它。

<code>nvme_rdma_qpair_process_completions()</code>：

1. 处理 qpair 连接或断连状态。
2. 轮询 per-qpair CQ 或共享 poll-group CQ。
3. 区分 SEND 与 RECV completion。
4. 重新 post response receive。
5. 检查 transport error 和 request timeout。
6. 完成 request，调用应用 callback。

发生 CQ、CM 或 remote failure 时，transport 将 qpair 标记为失败并断连。由于 shared CQ 中可能仍有 late completion，request object 必须在 drain 完成前保持有效。

## 11. SPDK 通用层与 RDMA 专属层的边界

| 能力 | 通用 SPDK 层，可供 URMA 复用 | RDMA 专属层，需要替换 |
|---|---|---|
| NVMe command/controller | Fabrics scan、Property、Identify、CONNECT 语义 | RDMA CM private data |
| Initiator 接口 | <code>spdk_nvme_transport_ops</code> | <code>rdma_ops</code> 的 verbs 实现 |
| Target 接口 | <code>spdk_nvmf_transport_ops</code> | <code>lib/nvmf/rdma.c</code> |
| Backend I/O | <code>spdk_nvmf_request_get_buffers()</code>、<code>spdk_nvmf_request_exec()</code> | RDMA data WR 构造 |
| Memory 抽象 | <code>spdk_memory_domain</code> | MR、lkey、rkey、<code>spdk_rdma_utils</code> |
| Poll group | SPDK poll-group/thread 模型 | CQ、SRQ、ibv WC |
| Discovery | 通用 discovery framework | RDMA TRTYPE 和 RDMA TSAS |
| Error status | NVMe completion status | CM event、WC status、QP drain |

URMA 应复用左列接口，重新实现右列机制。直接改造 <code>nvme_rdma.c</code> 或 <code>nvmf/rdma.c</code> 会把 URMA 绑定到 verbs、RDMA CM 和 keyed SGL，破坏独立 transport 目标。

## 12. 引入 NVMe/URMA 需要修改什么

### 12.1 增加独立 transport identity

新增：

~~~text
SPDK_NVME_TRANSPORT_NAME_URMA
SPDK_NVME_TRANSPORT_URMA
nvme_urma_ops
spdk_nvmf_transport_urma
~~~

同时修改 transport name parser、string conversion、Fabrics 分类、transport-ID comparison 和 RPC validation。内部临时值不得复用 <code>SPDK_NVME_TRANSPORT_RDMA</code>。

### 12.2 用 URMA bootstrap 替换 RDMA CM

RDMA 当前依赖：

~~~text
resolve_addr -> resolve_route -> rdma_connect/accept -> ESTABLISHED
~~~

URMA 需要：

~~~text
TCP/UVS bootstrap
  -> 交换 EID、Jetty/JFS/JFR descriptor 和 token
  -> import peer endpoint
  -> 预投递 control receive
  -> BOOTSTRAP_READY
  -> URMA transport connect
  -> NVMe Fabrics CONNECT
~~~

这部分负责建立 transport endpoint。标准 NVMe Fabrics CONNECT 仍由通用 NVMe/NVMf core 执行。

### 12.3 用 Jetty/JFC 替换 QP/CQ

| RDMA | URMA |
|---|---|
| <code>rdma_cm_id</code> | URMA endpoint/bootstrap context |
| ibverbs QP | Jetty 或 JFS/JFR |
| CQ / WC | JFC / <code>urma_cr_t</code> |
| SRQ | URMA receive pool 或共享 receive 机制 |
| SEND WR | URMA SEND work request |
| RDMA READ/WRITE WR | URMA READ/WRITE work request |

URMA poll group 轮询 JFC，通过 <code>user_ctx</code> 找到 request 或 receive object，并推进 transport 状态。

### 12.4 用 URMA region protocol 替换 keyed RDMA SGL

RDMA initiator 将 VA、length 和 rkey 放入 NVMe keyed SGL。URMA 不应在 wire 上伪装成 rkey。

建议使用两步机制：

1. initiator 通过 control message 注册 allocation，target import URMA segment。
2. target 返回 controller-scoped <code>region_key + generation</code>。

I/O command capsule 携带：

~~~text
region_key
generation
offset
length
access direction
standard NVMe command
~~~

target 通过 region table 找到 imported segment，再构造 URMA READ 或 WRITE。

region binding 使用 controller scope。controller 的 admin/control qpair 处理 register 和 unregister message，controller admin thread 独占 mutable imported-region table。import 成功后，admin thread 通过 SPDK thread message 向各 I/O qpair 发布 immutable binding handle；hot path 只持有 qpair-local handle 和 atomic reference，不修改 controller table。

unregister 或 provider invalidation 按以下顺序执行：

1. admin thread 将 binding 标记为 <code>INVALIDATING</code>，阻止该 controller 的所有 qpair 提交新 I/O。
2. admin thread 向持有 binding 的每个 poll group 发送 quiesce message。
3. 各 poll group drain 或 abort 相关 request，释放 binding reference，并返回 acknowledgement。
4. admin thread 收齐 acknowledgement 且引用计数归零后 unimport remote segment。
5. target 返回 unregister response；owner 收到 response 后才能释放 allocation。

### 12.5 扩展 memory domain，但不另造 I/O API

应用继续使用：

~~~text
spdk_nvme_ns_cmd_readv_ext()
spdk_nvme_ns_cmd_writev_ext()
memory_domain + memory_domain_ctx
~~~

URMA memory provider 将 HOST、CUDA、NPU 或 DMA-BUF allocation 转换为 local URMA registration。translation result 不再返回 lkey/rkey，而是返回 URMA local region、allocation identity、generation、offset 和 length。

现有 <code>spdk_memory_domain</code> 解决 address translation，但不足以独立管理 accelerator allocation 生命周期。URMA/XDS 层还需要：

- <code>describe</code>：识别已有 allocation；
- <code>allocation_id + generation</code>：防止 VA reuse；
- <code>acquire/release</code>：固定 allocation 生命周期；
- invalidation subscription；
- revoke-before-free；
- <code>sync_for_device/sync_for_cpu</code>；
- <code>direct_only</code> 与 staging counter。

### 12.6 Initiator request 的变化

保留：

- request pool 和 CID；
- command submission API；
- outstanding list；
- SEND/response 双 completion 的生命周期思想；
- SPDK completion callback。

替换或新增：

~~~text
RDMA memory translation       -> XDS/URMA provider translation
lkey/rkey keyed SGL           -> region binding + offset/length
ibv SEND WR                   -> URMA SEND
CQ polling                    -> JFC polling
RDMA RECV response            -> URMA response receive
                               + WAIT_REGION_REGISTER
                               + WAIT_SOURCE_SYNC
                               + WAIT_DEST_SYNC
~~~

NVMe WRITE 发送 capsule 前执行 source synchronization。NVMe READ 收到 response 后执行 destination synchronization，再调用应用 completion。

provider sync 返回 <code>0</code> 表示同步完成，返回 <code>-EINPROGRESS</code> 表示稍后异步调用 callback。provider 可以从任意 runtime thread 发起 callback，但 callback 只向 request owner SPDK thread 投递 message。owner thread 串行处理 sync completion、abort、资源释放和用户 callback，并通过 terminal flag 保证 exactly-once completion。

sync callback 尚未到达时发生 abort，request 进入 <code>ABORTING_SYNC</code>。owner thread 处理 callback 前，transport 必须继续持有 request object、allocation pin 和 region/binding reference，禁止释放或复用这些对象。

### 12.7 Target request 的变化

保留：

- <code>spdk_nvmf_request_get_buffers()</code>；
- <code>spdk_nvmf_request_exec()</code>；
- <code>req_complete</code>；
- 通用 controller、namespace 和 bdev；
- WRITE 先搬数据、READ 后搬数据的基本顺序。

替换：

~~~text
parse keyed SGL              -> validate region_key/generation/range
RDMA READ                    -> URMA READ
RDMA WRITE                   -> URMA WRITE
CQ/WC completion             -> JFC completion
QP send/read depth queue     -> URMA one-sided work queue depth
MR/rkey lifetime             -> local registration + controller binding
~~~

建议的 URMA target 主状态：

~~~text
FREE
  -> NEW
  -> NEED_BUFFER
  -> PULLING_INITIATOR_DATA       # NVMe WRITE
  -> EXECUTING_NVMF_CORE
  -> PUSHING_INITIATOR_DATA       # NVMe READ
  -> SENDING_RESPONSE
  -> FREE
~~~

URMA transport 仍然不能直接调用 bdev。WRITE 的 URMA READ 完成后调用 <code>spdk_nvmf_request_exec()</code>；READ 的 <code>req_complete</code> 触发 URMA WRITE。

### 12.8 增加显式 receive credit

RDMA QP/SRQ 和 RDMA CM negotiation 隐含了 receive capacity。URMA binding 需要在 transport protocol 中明确协商 command、response 和 control credit。

<code>CREDIT_UPDATE</code> 使用独立固定 receive，并且不消耗 ordinary control credit，以避免 credit update 互相等待。receive buffer pool 与 NVMe request pool 分离。

message credit 只限制 SEND/RECV message capacity。URMA READ/WRITE 还需要单独限制 outstanding one-sided work request 数量；两类计数不能合并。

### 12.9 Discovery、RPC 与构建

需要新增：

- <code>--with-urma</code>；
- UMDK header/library 检测；
- <code>nvmf_create_transport -t URMA</code>；
- URMA-specific JSON option；
- initiator/target transport registration；
- protocol、状态机和 disabled-build 单元测试。

获得正式 NVMe-oF TRTYPE 前，原型使用 direct connect，不在标准 discovery log 中发布临时数值。

## 13. NVMe/RDMA 与 NVMe/URMA 流程对照

| 阶段 | NVMe/RDMA | NVMe/URMA |
|---|---|---|
| Transport 建连 | RDMA CM | TCP/UVS bootstrap + URMA endpoint import |
| Queue object | QP | Jetty 或 JFS/JFR |
| Completion | CQ/WC | JFC/<code>urma_cr_t</code> |
| Command | RDMA SEND | URMA SEND |
| Buffer 授权 | address + rkey | region_key + generation + range |
| WRITE payload | target RDMA READ | target URMA READ |
| READ payload | target RDMA WRITE | target URMA WRITE |
| Backend | NVMf core + bdev | 原样复用 |
| Initiator API | SPDK NVMe API | 原样复用，并使用 memory domain |
| Accelerator memory | RDMA memory-domain/provider 路径 | HOST/GPU/NPU/XDS provider |
| Request 完成 | CQ SEND + RECV completion | JFC SEND/RECV/data completion + sync |

两者的 NVMe 语义相同，transport object、连接管理、memory authorization 和 completion source 不同。

## 14. 推荐实施顺序

### Phase 1：HOST buffer 与单 qpair

实现独立 URMA transport registry、bootstrap、Jetty/JFC、admin qpair 和单 I/O qpair。先完成无 data command、HOST WRITE 和 HOST READ。

### Phase 2：Region protocol 与 cache

实现 register/unregister、controller binding、generation、range/access 校验和 teardown。

### Phase 3：GPU GDR

接入 CUDA provider、异步 sync 和 invalidation，并在 SPDK <code>urma_perf</code> 中验证 WRITE 后 READ；UMDK 源码不修改。

### Phase 4：NPU/XDS 与社区能力

接入 NPU/XDS provider，增加多 qpair、shared poll group、错误注入、认证、discovery 和 long-run test。

## 15. 源码索引

- [NVMe/RDMA initiator transport](../lib/nvme/nvme_rdma.c)
  - <code>nvme_rdma_ctrlr_construct()</code>
  - <code>nvme_rdma_ctrlr_connect_qpair()</code>
  - <code>nvme_rdma_req_init()</code>
  - <code>nvme_rdma_qpair_submit_request()</code>
  - <code>nvme_rdma_qpair_process_completions()</code>
- [NVMe-oF/RDMA target transport](../lib/nvmf/rdma.c)
  - <code>nvmf_rdma_create()</code>
  - <code>nvmf_rdma_listen()</code>
  - <code>nvmf_rdma_connect()</code>
  - <code>nvmf_rdma_request_process()</code>
  - <code>nvmf_rdma_poller_poll()</code>
  - <code>nvmf_rdma_request_complete()</code>
- [NVMe transport operations](../include/spdk/nvme.h)
- [NVMf transport operations](../include/spdk/nvmf_transport.h)
- [SPDK NVMe-oF Target Programming Guide](https://spdk.io/doc/nvmf_tgt_pg.html)
- [NVMe specifications](https://nvmexpress.org/specifications/)
- [配套 NVMe/URMA 设计](nvme_urma_transport.md)
