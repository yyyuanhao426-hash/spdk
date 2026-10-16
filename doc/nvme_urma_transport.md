# NVMe over URMA 传输设计

## 文档状态

本文定义 SPDK 中独立 NVMe over Unified Remote Memory Access（NVMe/URMA）传输的初始设计，覆盖 SPDK NVMe initiator、NVMe-oF target、异构内存注册和 SPDK GPU 性能测试程序。本文用 XDS 代称后续 SSD-to-accelerator 数据路径及其 memory provider 约定。

文中的“必须”“禁止”“应当”和“可以”分别表示强制约束、禁止行为、推荐方案和可选方案。本文是开发约定，并非已发布的 NVMe 传输标准。transport type 数值、discovery 格式和 XDS provider ABI 在社区形成稳定定义前均为临时方案。

## 当前分支实现快照

当前 `urma_enable` 分支已经提供第一版可联调代码，而不是仅有接口占位：

- SPDK initiator 与 NVMf target 均注册独立的 `URMA` transport，没有调用 NVMe/RDMA transport。
- TCP socket 当前只承担 endpoint 描述符、NVMe command capsule 和 completion 的控制面交换；有数据的 command 使用 URMA READ/WRITE 搬运 payload。
- target 继续调用原有 `spdk_nvmf_request_exec()`，因此 NVMe controller 状态机、namespace、bdev 和 SSD 完成路径保持不变；URMA transport 只管理其自身的“拉取数据—执行 bdev—推送数据”阶段。
- 增加 HOST、CUDA、ROCm、NPU 和 XDS memory provider 抽象。HOST 可直接注册；异构内存由外部 provider 负责 pin/unpin 和可选 DMA-BUF 导出，再由公共层完成 URMA segment 注册。
- SPDK 增加 `build/examples/urma_perf`，可用 CUDA 显存直接连接 SPDK target，并统计正确性、时延、IOPS 与带宽；UMDK 源码不修改。

本快照属于端到端 MVP：仅支持单个连续 SGL、每个 qpair 一个 Jetty、最大 I/O 默认 128 KiB，尚未实现注册缓存、重连、超时恢复、多 SGL、协议 golden test 和完整性能调优。当前 UMDK 的 DMA-BUF 接口仍可能返回不支持，此时会回退到该分支已有的 `is_gpu_seg` peer-memory 注册路径；是否为真正零 HOST staging 必须在目标硬件上验证。

### 编译开关

~~~bash
./configure --with-urma=/opt/umdk
make -j
~~~

关闭该功能使用 `--without-urma`。配置项为 `CONFIG_URMA` 和 `CONFIG_URMA_DIR`，关闭时不会编译 URMA transport，也不会引入 `liburma`。

### 运行参数

默认值参考 Mooncake 当前 URMA 实现：RM 模式、2 个 JFC、JFC 深度 4096、Jetty 深度 2048、priority 15、max_sge 5、rnr_retry 7、err_timeout 17、token `0xACFE`，active port 默认自动选择。

SPDK 使用以下环境变量；为便于与 Mooncake 联调，四个 Mooncake 同名变量也作为低优先级兼容入口：

| SPDK 参数 | Mooncake 兼容参数 | 默认值 |
|---|---|---|
| `SPDK_URMA_TRANS_MODE` | `MC_URMA_TRANS_MODE` | `RM` |
| `SPDK_URMA_ACTIVE_PORT` | `MC_URMA_ACTIVE_PORT` | 自动选择 active port |
| `SPDK_URMA_BONDING_BALANCE` | `MC_URMA_BONDING_BALANCE` | `false` |
| `SPDK_URMA_BONDING_MULTIPATH_ENABLE` | `MC_URMA_BONDING_MULTIPATH_ENABLE` | `false` |
| `SPDK_URMA_DEV_NAME` | 无 | 第一个匹配设备 |
| `SPDK_URMA_EID_INDEX` | 无 | `0`，不存在时使用首个 EID |
| `SPDK_URMA_JFC_COUNT` | 无 | `2` |
| `SPDK_URMA_JFC_DEPTH` | 无 | `4096` |
| `SPDK_URMA_JETTY_COUNT` | 无 | `1` |
| `SPDK_URMA_JETTY_DEPTH` | 无 | `2048` |
| `SPDK_URMA_MAX_IO_SIZE` | 无 | `131072` |

NVMf target 的 transport-specific JSON 还可覆盖 `dev_name`、`trans_mode`、`active_port`、`eid_index`、`jfc_count`、`jfc_depth`、`jetty_count`、`jetty_depth`、`bonding_balance` 和 `bonding_multipath`。

### 最小联调示例

target 侧在创建 bdev 与 subsystem 后创建 URMA transport 和 listener：

~~~bash
scripts/rpc.py nvmf_create_transport -t URMA
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 \
    -t URMA -a 0.0.0.0 -s 4420
~~~

initiator 测试侧：

~~~bash
build/examples/urma_perf \
    -r 'trtype:URMA adrfam:IPv4 traddr:192.0.2.10 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' \
    -w write -o 4096 -T 1 -b 16 -t 10 -g 0 -n 1 -l 0

build/examples/urma_perf \
    -r 'trtype:URMA adrfam:IPv4 traddr:192.0.2.10 trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' \
    -w read -o 4096 -T 1 -b 16 -t 10 -g 0 -n 1 -l 0
~~~

完整参数、破坏性警告和结果判据见 [NVMe/URMA GPU 到远端 SSD 测试指南](nvme_urma_gpu_perf.md)。

## 1. 目标与范围

NVMe/URMA 通过 URMA 连接 SPDK NVMe initiator 与 SPDK NVMe-oF target，并通过新的 transport binding 传输标准 NVMe command 和 completion。该实现不映射到 NVMe/RDMA，也不依赖 <code>libibverbs</code> 或 <code>rdma_cm</code>。

第一阶段包含四项目标：

1. 为 SPDK NVMe initiator 增加独立 <code>URMA</code> transport。
2. 为 SPDK NVMe-oF target 增加独立 <code>URMA</code> transport。
3. 通过统一 memory provider 接口支持 HOST、GPU、NPU 和后续 XDS buffer，包括 DMA-BUF。
4. 在 SPDK 中增加 <code>urma_perf</code>，完成 GPU Direct RDMA（GDR）+ NVMe/URMA 端到端存储测试，不修改 UMDK。本文沿用 GDR 表示 URMA device 直接访问 GPU buffer、且 payload 不经过 HOST staging 的能力。

第一阶段的数据路径为：

~~~text
GPU、NPU 或 HOST buffer
        |
        | URMA READ / WRITE
        v
SPDK target HOST buffer
        |
        | SPDK NVMf core / bdev I/O
        v
NVMe SSD
~~~

当 URMA device 能够直接访问 accelerator buffer 时，该路径消除 initiator 侧 HOST bounce buffer。target 侧仍保留 HOST buffer。存储侧 PCIe P2P DMA 和 Mooncake 修改不在第一阶段范围内。

## 2. 设计要求

| 编号 | 要求 | 设计方案 |
|---|---|---|
| R1 | URMA 与 RDMA 独立 | 独立 transport type、wire protocol、状态机和源文件 |
| R2 | accelerator direct 模式不经过 HOST staging | 显式 memory domain 和 <code>direct_only</code> |
| R3 | 同一 API 支持 HOST、GPU、NPU 和 XDS | provider-based 异构内存描述符 |
| R4 | 多次 I/O 复用注册 | local registration cache + controller peer binding |
| R5 | 数据移动完成后才完成 NVMe command | 状态机统一管理 URMA 与 NVMf completion |
| R6 | 不影响现有 transport | <code>--with-urma</code> 和独立 transport 注册 |
| R7 | 测试发现静默 fallback | 注册统计和 zero-staging 断言 |

## 3. 术语

- **GDR（GPU Direct RDMA）**：设备直接访问 GPU buffer 的数据路径。本文沿用 GDR 表示 GPU buffer 与 URMA device 之间不经过 HOST staging 的直接访问能力。
- **XDS**：面向 SSD 与 accelerator 的后续数据路径及 memory provider 约定。本文只定义与其对接所需的临时抽象。
- **EID（Endpoint Identifier）**：URMA/UB endpoint 的标识符，用于选择本地或远端 endpoint。
- **Jetty**：管理已提交 I/O task 或 received message 的 URMA queue，可视为 URMA command 的执行端口。
- **JFS（Jetty for Send）**：提交 DMA task 或发送 message 的对象。
- **JFR（Jetty for Receive）**：准备 message receive resource 的对象。
- **JFC（Jetty for Completion）**：保存 JFS/JFR completion record 的 completion queue。
- **Qpair**：NVMe submission/completion queue pair。一个 NVMe/URMA qpair 对应一个有序 command stream 和 response stream。
- **UDMA provider**：UMDK 中承接具体硬件平台的用户态 URMA provider/data-path 实现。它与 URMA API 层不同。
- **UVS**：UMDK 的 URMA control-plane service，负责 endpoint 和 transport path 等控制面资源的协商或管理。具体接入方式仍待确定。
- **UPI**：URMA control plane 使用的 port/path identifier。它在 NVMe transport address 中的最终编码仍待确定。
- **TSAS（Transport Specific Address Subtype）**：NVMe-oF discovery entry 中描述 transport-specific address 格式的字段。
- **Memory region**：向 URMA 注册的 allocation 或 allocation range。wire 使用 controller-scoped <code>region_key</code> 和 <code>generation</code> 标识 peer binding。
- **Direct mode**：URMA device 直接访问应用 buffer，payload 不复制到 HOST staging buffer。

## 4. 总体架构

~~~text
+------------------------- Initiator --------------------------+
| Application / SPDK urma_perf                               |
|   -> SPDK readv_ext / writev_ext + spdk_memory_domain       |
|   -> lib/nvme/nvme_urma.c                                   |
|   -> lib/urma/ + lib/xds/                                   |
+------------------------------|-------------------------------+
                               | URMA
+------------------------------|-------------------------------+
| lib/nvmf/urma.c                                              |
|   -> SPDK NVMf core -> SPDK bdev -> NVMe SSD                |
+--------------------------- Target ---------------------------+
~~~

建议的代码布局：

~~~text
include/spdk/nvme_urma.h
include/spdk/xds_memory.h

lib/urma/
    urma_env.c
    urma_endpoint.c
    urma_memory.c
    urma_poll_group.c
    urma_protocol.c
    urma_internal.h

lib/xds/
    xds_memory.c
    xds_memory_host.c
    xds_share_dmabuf.c
    xds_memory_cuda.c
    xds_memory_npu.c

lib/nvme/nvme_urma.c
lib/nvmf/urma.c
test/unit/lib/{urma,nvme/nvme_urma.c,nvmf/urma.c,xds}/
~~~

<code>lib/nvme</code> 和 <code>lib/nvmf</code> 管理 NVMe 状态；<code>lib/urma</code> 管理 URMA object、protocol 和 polling；memory provider 管理 CUDA、NPU 等 runtime。

## 5. Transport identity 与 discovery

SPDK 增加独立 transport name：

~~~c
#define SPDK_NVME_TRANSPORT_NAME_URMA "URMA"
SPDK_NVME_TRANSPORT_URMA = 4098, /* SPDK 内部临时值 */
~~~

4098 不是 wire-level TRTYPE，禁止 alias 到 <code>SPDK_NVME_TRANSPORT_RDMA</code>。当前 <code>spdk_nvme_trtype_is_fabrics()</code> 不能自动识别 4098，因此需要同步修改：

- Fabrics transport 分类；
- transport name 解析和字符串转换；
- transport ID 比较；
- RPC JSON 校验；
- probe/connect 校验。

测试必须证明 URMA 进入通用 Fabrics controller 初始化，并且 4098 不会被截断后写入 8-bit discovery TRTYPE。

第一阶段使用 direct connect：

~~~text
trtype:URMA adrfam:IB traddr:<eid> trsvcid:<service> subnqn:<nqn>
~~~

获得稳定 TRTYPE 和 transport-specific address subtype 前，标准 discovery response 不发布 URMA listener。社区后续可以增加正式的 <code>SPDK_NVMF_TRTYPE_URMA</code>，但不能改变本文定义的传输协议。

## 6. URMA 连接模型

每个进程为选定 URMA device 和 EID index 创建一个 URMA context。每个 SPDK poll group 创建或关联 JFC。每个 qpair 持有 send/receive object 和连接状态。第一阶段使用 UDMA provider 支持的 reliable connected mode。

### 6.1 连接自举

URMA SEND 在 import remote endpoint descriptor 前无法承载该 descriptor，因此需要 bootstrap channel。原型使用 TCP control socket；社区版本可以替换为 UVS 或原生 URMA connection service。bootstrap 只传输控制元数据。

~~~text
TCP_CONNECT / TCP_ACCEPT
  -> 交换 version、nonce、EID、transport mode、device identity
  -> 双方创建本地 Jetty 或 JFS/JFR 和 JFC
  -> 交换 serialized endpoint descriptor 和 import token
  -> 校验 peer identity、mode、length 和 nonce binding
  -> import peer endpoint
  -> 分配并预投递 control receive buffer
  -> 交换 BOOTSTRAP_READY
  -> 开始 URMA transport connect
  -> 发送标准 NVMe Fabrics CONNECT
~~~

每一步必须设置 timeout 和逆序清理路径。双方完成 <code>BOOTSTRAP_READY</code> 前，target 禁止创建 <code>spdk_nvmf_qpair</code>。

### 6.2 能力与 credit

handshake 至少协商 protocol version、transport mode、queue depth、maximum capsule/I/O/SGE、inline limit、region register/invalidate 和 direct memory type。

URMA SEND 要求 peer 预先 post receive。每个 qpair 分别维护 command、response 和 ordinary control credit：

- command credit 对应 <code>CAPSULE_CMD</code>；
- response credit 对应 <code>CAPSULE_RSP</code>；
- ordinary control credit 对应 region、error 和 disconnect message；
- <code>CREDIT_UPDATE</code> 使用独立固定 receive，不参与 ordinary credit 计算。

credit 以 message 数计数。sender 按 <code>initial + returned - sent</code> 计算可用量，只接受递增的 64-bit 累计 returned counter。固定的 <code>CREDIT_UPDATE</code> receive 在处理前先 repost，消费该消息不产生新 credit。普通 control traffic 禁止消费最后一个 credit；该 credit 专供 disconnect 或 terminal error。

receive buffer pool 与 NVMe request pool 分离，防止 request pool 满时阻塞释放旧 request 所需的 control message。

## 7. Wire protocol

多字节字段使用 little-endian。结构体使用定宽整数、packed layout 和 compile-time size assertion。

~~~c
struct nvme_urma_msg_hdr {
    uint8_t  version;
    uint8_t  type;
    uint16_t flags;
    uint16_t header_len;
    uint16_t payload_offset;
    uint32_t total_len;
    uint16_t qid;
    uint16_t cid;
    uint32_t sequence;
    uint32_t status;
    uint64_t opaque;
};

enum nvme_urma_msg_type {
    NVME_URMA_MSG_TRANSPORT_CONNECT_REQ = 0x01,
    NVME_URMA_MSG_TRANSPORT_CONNECT_RSP = 0x02,
    NVME_URMA_MSG_CAPSULE_CMD           = 0x10,
    NVME_URMA_MSG_CAPSULE_RSP           = 0x11,
    NVME_URMA_MSG_REGION_REGISTER_REQ   = 0x20,
    NVME_URMA_MSG_REGION_REGISTER_RSP   = 0x21,
    NVME_URMA_MSG_REGION_UNREGISTER_REQ = 0x22,
    NVME_URMA_MSG_REGION_UNREGISTER_RSP = 0x23,
    NVME_URMA_MSG_CREDIT_UPDATE         = 0x24,
    NVME_URMA_MSG_DISCONNECT            = 0x30,
    NVME_URMA_MSG_ERROR                 = 0x31,
};
~~~

connect request/response 携带 version、mode、queue limit、direct memory capability 和三类 initial credit。major version 不一致时拒绝连接。协议第一版不分片 control message。

### 7.1 Command 与 completion capsule

~~~c
struct nvme_urma_capsule_cmd {
    struct nvme_urma_msg_hdr hdr;
    uint32_t region_key;
    uint32_t generation;
    struct spdk_nvme_cmd cmd;
};

struct nvme_urma_capsule_rsp {
    struct nvme_urma_msg_hdr hdr;
    struct spdk_nvme_cpl cpl;
};
~~~

<code>hdr.cid</code> 必须等于 NVMe command CID。direct data command 使用 transport SGL：

~~~text
type       = SPDK_NVME_SGL_TYPE_TRANSPORT_DATA_BLOCK
subtype    = SPDK_NVME_SGL_SUBTYPE_TRANSPORT
address    = registered region 内的 byte offset
length     = payload length
~~~

<code>region_key</code> 和 <code>generation</code> 位于 capsule header，URMA metadata 不进入 NVMe SGL，也不使用 RDMA key 语义。第一版每个 command 支持一个 contiguous SGL。

### 7.2 Region protocol

region register request 携带 <code>allocation_id</code>、base、length、generation、access、memory type、device ID、serialized segment descriptor 和 token。序列化格式必须独立于 <code>urma_seg_t</code> 的内存布局。

target import segment 后分配 controller-scoped <code>region_key</code>：

- NVMe WRITE 需要 <code>REMOTE_READ</code>，target 从 initiator 拉取数据。
- NVMe READ 需要 <code>REMOTE_WRITE</code>，target 向 initiator 写入数据。

owner 释放 allocation 前，通过 controller admin/control qpair 发送 unregister。receiver 将 binding 标记为 <code>INVALIDATING</code>，阻止全部 qpair 提交新 I/O，等待引用归零并 unimport，然后返回 response。owner 收到 response 前必须保持 allocation 有效。

## 8. 异构内存与 XDS

memory type 与 sharing method 分开表示：

~~~c
enum spdk_xds_memory_type {
    SPDK_XDS_MEM_HOST, SPDK_XDS_MEM_CUDA, SPDK_XDS_MEM_ROCM,
    SPDK_XDS_MEM_NPU, SPDK_XDS_MEM_VENDOR = 0x8000,
};

enum spdk_xds_share_method {
    SPDK_XDS_SHARE_HOST_VA,
    SPDK_XDS_SHARE_PEER_MEMORY,
    SPDK_XDS_SHARE_DMABUF,
    SPDK_XDS_SHARE_PROVIDER_PRIVATE,
};

struct spdk_xds_memory_desc {
    size_t size;
    enum spdk_xds_memory_type type;
    enum spdk_xds_share_method share_method;
    void *va;
    uint64_t length;
    uint64_t allocation_id;
    uint32_t generation;
    uint32_t device_id;
    int dmabuf_fd;
    uint64_t dmabuf_offset;
    void *provider_ctx;
};
~~~

公共接口不暴露 <code>is_gpu_seg</code>。provider interface 至少包含：

~~~text
probe
alloc / describe / free
acquire / release
subscribe_invalidation / unsubscribe_invalidation / invalidation_done
query
register_urma / unregister_urma
sync_for_device / sync_for_cpu
~~~

已有 allocation 的 descriptor 必须由 provider 的 <code>describe</code> 生成。应用不能自行构造 <code>allocation_id</code> 或 <code>generation</code>。identity 在一次 allocation 生命周期内保持稳定；VA 或 identity 重用时必须增加 generation。

### 8.1 生命周期与失效

<code>acquire</code> 固定 allocation 生命周期，<code>release</code> 释放引用。存在 acquire reference 或 peer binding 时，<code>free</code> 返回 <code>-EBUSY</code>。

provider invalidation 使用 revoke-before-free：

1. provider 通知 transport 即将撤销 allocation，并保持 mapping 有效。
2. transport 阻止新 I/O，drain 或 abort 现有 I/O。
3. transport unregister binding、释放 allocation，并调用 <code>invalidation_done</code>。
4. provider 才能 unmap、free 或复用地址和 identity。

设备 reset 无法保留 mapping 时，provider 必须先原子撤销 URMA access，使 outstanding operation 失败，并在 <code>invalidation_done</code> 前禁止 identity 重用。

同步函数返回 <code>0</code> 表示完成，返回 <code>-EINPROGRESS</code> 表示稍后调用一次 callback。只有 sync 返回 <code>-EINPROGRESS</code>、且 abort 发生在 callback 到达前时，request 才进入 <code>ABORTING_SYNC</code>。callback 在 owner SPDK thread 完成串行处理前，transport 必须继续持有 request object、allocation pin 和对应 region/binding reference，禁止释放或复用其中任何对象。

provider 可以从任意 runtime thread 发起 callback，但 callback 只能向 request owner SPDK thread 投递 message。状态转换、abort、资源释放和用户 callback 均在 owner thread 串行执行，并通过 terminal flag 保证 exactly-once completion。

### 8.2 SPDK memory domain 与两级 cache

应用通过 <code>readv_ext</code>/<code>writev_ext</code> 的 <code>memory_domain</code> 和 <code>memory_domain_ctx</code> 传递异构 buffer。URMA controller 通过 <code>ctrlr_get_memory_domains</code> 暴露 destination domain。translation result 标识 registered URMA region、offset 和 length，且生命周期覆盖 NVMe request。

local registration cache 按 URMA device 和 protection context 建立，key 包含 provider、allocation identity、generation、range、device、access 和 share method。多个 controller 可以复用 local registration；每个 controller 单独建立 peer import binding，并为其分配 <code>region_key</code>。该 controller 的全部 I/O qpair 共享 binding。

local entry 持有 provider acquire reference 和 URMA registration；controller binding 持有 import reference；outstanding command 持有 binding reference。cache key 禁止只使用 raw VA。

### 8.3 Direct-only

<code>SPDK_XDS_IO_DIRECT_ONLY</code> 要求 provider 在需要 HOST staging 时返回失败。transport 至少输出：

~~~text
direct_registered_bytes
host_registered_bytes
host_staged_bytes
registration_cache_hits
registration_cache_misses
provider_invalidations
~~~

GDR/NPU-direct 测试只有在测量区间内 <code>host_staged_bytes == 0</code> 时通过。

## 9. Initiator 数据路径

<code>nvme_urma_qpair_submit_request()</code>：

1. 按 CID 分配 request。
2. 通过 SPDK memory domain 解析 payload。
3. 获取或注册 URMA region。
4. NVMe WRITE 在暴露 source region 前执行 <code>sync_for_device</code>。
5. 构造 transport SGL 和 capsule。
6. post response receive，并通过 URMA SEND 发送 capsule。
7. 等待 data movement 和 response。
8. NVMe READ 在调用应用 callback 前执行 <code>sync_for_cpu</code>。

~~~text
FREE -> PREPARING -> WAIT_REGION_REGISTER
     -> WAIT_SOURCE_SYNC -> SENDING_CMD -> WAIT_DATA_AND_RSP
     -> WAIT_DEST_SYNC -> COMPLETING -> FREE

普通 active state -> ABORTING -> FAILED -> FREE
存在未完成 sync callback -> ABORTING_SYNC -> FAILED -> FREE
~~~

request 分别跟踪 command SEND、response RECV、provider sync 和 region lifetime。completion poller 通过 <code>urma_cr_t.user_ctx</code> 找到 request，并遵守 SPDK <code>max_completions</code> 限制。

## 10. Target 数据路径

每个 target poll group 持有 JFC、capsule receive、request pool、target data buffer，以及 qpair-local immutable region binding reference。

target controller admin thread 独占 mutable imported-region table。register/unregister 在该 thread 执行。admin thread 通过 SPDK message 向各 I/O qpair 发布 immutable handle。invalidation 向所有持有 binding 的 poll group 发送 quiesce message；收到全部 acknowledgement 后才能 unimport。qpair migration 先获取 destination reference，再释放 source reference。

transport 必须把 request 交给通用 NVMf core，禁止直接调用 bdev API。

### 10.1 NVMe WRITE

~~~text
接收 capsule并校验 region/range/access
  -> spdk_nvmf_request_get_buffers()
  -> URMA READ: initiator buffer -> target buffer
  -> 等待 URMA READ completion
  -> spdk_nvmf_request_exec()
  -> NVMf core 完成 backend I/O
  -> req_complete 发送 NVMe completion
~~~

### 10.2 NVMe READ

~~~text
接收 capsule 并校验 region/range/access
  -> spdk_nvmf_request_get_buffers()
  -> spdk_nvmf_request_exec()
  -> NVMf core 完成 backend I/O
  -> req_complete 投递 URMA WRITE: target buffer -> initiator buffer
  -> 等待 URMA WRITE completion
  -> 发送 NVMe completion
~~~

successful WRITE completion 表示 NVMf core 已完成 backend I/O；successful READ completion 表示 target 已完成 URMA WRITE。transport failure 对每个 request 只执行一次 complete 或 abort。

Reservations 由通用 core/backend 处理；authentication 使用 NVMf hook；multipath 由 initiator policy 管理；fused command 只有在 transport 满足 ordering/capability 后才启用。第一阶段使用 direct connect 和单 path 测试。

## 11. 错误、恢复与安全

transport 区分 protocol、region、URMA local、URMA remote、target resource、backend 和 provider error。timeout 将 qpair 转为 <code>FAILED</code>，阻止新提交并开始清理。状态不确定的 WRITE 不执行 replay。

teardown 顺序：

1. 停止新 request。
2. abort 或 drain outstanding request。
3. 停止 region traffic。
4. unregister imported region。
5. drain URMA completion。
6. 销毁 qpair 和 poll-group resource。
7. 释放 URMA context。

安全要求：

- 使用不可预测的 controller-scoped region key；
- 将 region 绑定到一个 controller 及其 qpair；
- 每个 command 校验 direction、range、key 和 generation；
- disconnect 时删除 imported-region state；
- 拒绝未知版本或非法长度的 descriptor；
- 常规日志不输出 token 和完整 accelerator address；
- 接收 command 前执行 SPDK subsystem host access control。

## 12. SPDK 构建与修改点

~~~text
./configure --with-urma[=<path>]
CONFIG_URMA=y
~~~

CUDA 和 NPU provider 使用独立 option，使 HOST-only build 不依赖 accelerator SDK。

| 位置 | 修改 |
|---|---|
| <code>include/spdk/nvme.h</code> | URMA transport identity 和 public hook |
| <code>lib/nvme/nvme.c</code> | 解析和输出 <code>trtype:URMA</code> |
| transport helper | Fabrics 分类和无截断比较 |
| JSON/RPC path | 按 name 接受 URMA，拒绝非法 discovery serialization |
| <code>lib/nvme/nvme_transport.c</code> | 注册 initiator constructor |
| <code>include/spdk/dma.h</code> | URMA translation result 或 opaque extension |
| <code>lib/nvmf/transport.c</code> | 注册 target transport |
| NVMf RPC module | 解析 URMA-specific option |
| configure/makefile | 检测 UMDK 和可选 provider |
| tests | protocol、状态机、错误和 disabled build |

补丁不修改 RDMA 源文件。共享 helper 只有在接口保持 transport-neutral 时才进入 NVMe-oF core。

## 13. SPDK <code>urma_perf</code> 验证

SPDK <code>nvmf_tgt</code> 作为 storage target，SPDK
<code>build/examples/urma_perf</code> 作为 NVMe/URMA initiator。程序使用 CUDA
显存、一个 qpair/worker 和可配置 outstanding 深度，输出正确性、带宽、IOPS 与
时延百分位。DMA-BUF 和 peer-memory 注册次数由 NVMe/URMA 公共层统计。

详细命令见 [NVMe/URMA GPU 到远端 SSD 测试指南](nvme_urma_gpu_perf.md)。

## 14. 待确定问题

1. EID、UPI、<code>traddr</code>、<code>trsvcid</code> 和 TSAS 的最终编码。
2. initiator 是否直接与 UVS 通信，以及哪些 Jetty identifier 进入 wire。
3. 第一种 transport mode 的 reliability、ordering、reconnection 和 message limit。
4. 正式 8-bit TRTYPE 与 discovery address subtype。
5. XDS SDK 发布后的 provider ABI 收敛方式。
6. UMDK 用通用 memory type 或 DMA-BUF contract 替代 <code>is_gpu_seg</code>。
7. inline WRITE data 和小型 admin payload 的后续定义。

## 15. 实施阶段与验收

### Phase 0：协议骨架

增加 transport enum、serializer/parser、bootstrap、credit 和 malformed-message 测试。

**验收：**双方逐字节生成和解析 golden message。

### Phase 1：HOST memory

实现 endpoint、SEND/RECV、JFC polling、admin qpair 和一个 I/O qpair。

**验收：**malloc bdev 与 NVMe bdev 均通过 HOST READ/WRITE 校验。

### Phase 2：Registration cache

实现 register/unregister、两级 cache、generation、invalidation、provider describe 和异步 sync。

**验收：**重复 I/O 复用注册，强制 invalidation 无法访问 stale allocation。

### Phase 3：GPU direct

增加 CUDA provider 和 <code>--memory=cuda --direct-only</code>。

**验收：**WRITE + READ 通过逐字节校验，测量区间内 HOST staging 为零。

### Phase 4：NPU 与 XDS

增加 NPU provider 或集成正式 XDS provider，并复用相同 NVMe submission API。

**验收：**HOST、CUDA 和 NPU 产生一致存储语义。

### Phase 5：社区合入

增加多 qpair、shared poll group、错误注入、timeout、disconnect 和 long-run test，并解决 TRTYPE/discovery。

**验收：**URMA enabled/disabled 两种构建均通过 SPDK format、unit、functional 和 hardware test。

## 16. 参考资料

- <code>include/spdk/nvme.h</code>
- <code>include/spdk/nvmf_transport.h</code>
- <code>include/spdk/nvme_spec.h</code>
- <code>include/spdk/dma.h</code>
- <code>lib/nvme/nvme_transport.c</code>
- <code>lib/nvmf/transport.c</code>
- <code>lib/nvme/nvme_rdma.c</code>，只参考 request/poll-group 结构，不复用 wire binding。
- <code>lib/nvmf/rdma.c</code>，只参考 target request-state 覆盖范围。
- <code>UMDK_tool_netlab/src/urma/lib/urma/core/include/urma_api.h</code>
- <code>UMDK_tool_netlab/src/urma/lib/urma/core/urma_dp_api.c</code>
- <code>UMDK_tool_netlab/src/urma/tools/urma_perftest/perftest_memory.h</code>，仅作为既有 GPU memory 行为参考，不修改。
- [NVMe specifications](https://nvmexpress.org/specifications/)
- [SPDK NVMe-oF Target Programming Guide](https://spdk.io/doc/nvmf_tgt_pg.html)
