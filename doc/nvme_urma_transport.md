# NVMe over URMA 传输设计

## 文档状态

本文定义 SPDK 中独立 NVMe over Unified Remote Memory Access（NVMe/URMA）传输的初始设计，覆盖 SPDK NVMe initiator、NVMe-oF target、异构内存注册和 SPDK GPU 性能测试程序。本文用 XDS 代称后续 SSD-to-accelerator 数据路径及其 memory provider 约定。

文中的“必须”“禁止”“应当”和“可以”分别表示强制约束、禁止行为、推荐方案和可选方案。本文是开发约定，并非已发布的 NVMe 传输标准。transport type 数值、discovery 格式和 XDS provider ABI 在社区形成稳定定义前均为临时方案。

## 当前分支实现快照

当前 `urma_enable_v2` 分支已经提供可联调代码，而不是仅有接口占位：

- SPDK initiator 与 NVMf target 均注册独立的 `URMA` transport，没有调用 NVMe/RDMA transport。
- TCP socket 始终承担 endpoint 描述符交换；NVMe command capsule 和 completion 可配置为 TCP 或 URMA SEND/RECV，有数据的 command 使用 URMA READ/WRITE 搬运 payload。
- target 继续调用原有 `spdk_nvmf_request_exec()`，因此 NVMe controller 状态机、namespace、bdev 和 SSD 完成路径保持不变；URMA transport 只管理其自身的“拉取数据—执行 bdev—推送数据”阶段。
- 增加 HOST、CUDA、ROCm、NPU 和 XDS memory provider 抽象。HOST 可直接注册；异构内存由外部 provider 负责 pin/unpin 和可选 DMA-BUF 导出，再由公共层完成 URMA segment 注册。
- SPDK 增加 `build/examples/urma_perf`，可用 CUDA 显存直接连接 SPDK target，并统计正确性、时延、IOPS 与带宽；UMDK 源码不修改。

本快照属于端到端 MVP：仅支持单个连续 SGL，最大 I/O 默认 128 KiB；每个 endpoint 的 Jetty 数可配置，且已经提供本地注册缓存、target import 哈希缓存和 target iobuf 整池注册。重连、超时恢复、多 SGL、协议 golden test 和完整性能调优仍未实现。当前 UMDK 的 DMA-BUF 接口仍可能返回不支持，此时会回退到该分支已有的 `is_gpu_seg` peer-memory 注册路径；是否为真正零 HOST staging 必须在目标硬件上验证。

### 编译开关

~~~bash
./configure --with-urma=/opt/umdk
make -j
~~~

关闭该功能使用 `--without-urma`。配置项为 `CONFIG_URMA` 和 `CONFIG_URMA_DIR`，关闭时不会编译 URMA transport，也不会引入 `liburma`。

`--with-urma` 默认启用 HOST 注册和基础 transport；bonding 还要求下文的 rjetty 导出接口，不要求定制
`is_gpu_seg` 字段、`urma_register_seg_dmabuf` 符号或 CUDA provider。异构内存注册
通过两个默认关闭的独立选项开启，且均要求 `--with-urma`：

| 配置选项 | 配置宏 | 选定 UMDK 的要求 |
|---|---|---|
| `--with-urma-peermem` | `SPDK_CONFIG_URMA_PEERMEM` | `urma_seg_cfg_t` 含 `is_gpu_seg` |
| `--with-urma-dmabuf` | `SPDK_CONFIG_URMA_DMABUF` | 声明并导出 `urma_register_seg_dmabuf` |

也可使用 `--without-urma-peermem`、`--without-urma-dmabuf` 显式关闭。
peermem-only 构建不引用 DMA-BUF 注册符号；dmabuf-only 构建不访问 GPU 扩展字段，
注册失败后不会把 GPU 地址当 HOST 注册。两者同时开启时保留 DMA-BUF 到 peermem
的降级行为。configure 检查编译/链接条件，真实注册能力仍取决于运行时驱动；
只有 stub 符号不能证明支持真实 DMA-BUF。

这些选项不依赖 `--with-cuda`（该选项控制 CUDA accel module）。CUDA 的 allocation
保活和可选 fd 导出继续由外部 memory provider 实现。未启用异构注册时，provider
注册和非 HOST 内存注册返回 `-ENOTSUP`。当前 target 本地使用 HOST buffer，仍能
import 远端 GPU 段，不要求 target 因此开启本地异构内存注册。

从此前版本升级时，需要为 GPU initiator 显式添加上述选项。调用方可以检查安装的
`spdk/config.h` 获取构建能力；Mooncake 的 `USE_NOF_GPU_DIRECT` 不会裁剪已经生成的
SPDK archive，必须重新构建 SPDK 并重新链接调用方。同进程使用的 UMDK 头文件、
core/provider 库及公开结构 ABI 仍必须兼容；这些开关不修复 bonding 拓扑 ABI。

不依赖 GPU 硬件的构建/注册回归测试：

~~~bash
python3 test/urma/registration_build.py --umdk /path/to/gpu-capable/UMDK
~~~

该测试从指定 SDK 构造缺字段/缺符号的头文件版本，编译实际注册代码和测试工具，
检查四种构建组合的符号依赖、注册/降级/资源清理，以及 configure 能力探针。

### Bonding 完整上下文与光互联（wire v6）

本实现参考 Mooncake `mooncake-vesegment@4c0ea28c` 的 Transfer Engine：
`UrmaEndpoint::JettyRjettyBlobs()`、`UrmaContext::buildLocalBufferDesc()` /
`retrieveRemoteSeg()`、`selectBondingChipIds()`。SPDK 保留原有 NVMe 请求执行流程，
在 transport 内使用相同的 provider 上下文与选路策略：

- HELLO 交换 `urma_get_rjetty()` 返回的完整上下文，按每个 jetty 的实际长度传输。
- 注册内存时调用 `urma_get_seg_ctx()`，缓存完整导出结果。command capsule 携带完整
  context，target 导入完整副本；缓存身份同时比较基础描述与完整 context 字节。
  与 TE 一样，导入描述保留到 `urma_unimport_seg()` 后释放。
- 光互联 WR 选择 `{local_chip, remote_chip}`；电互联保持 `{remote_chip, remote_chip}`。
  显式亲和仅用于 bonding；芯片 ID 无法确定时使用 provider 默认路径。
- bonding 设备设置 `SPDK_URMA_BONDING_BALANCE=1`、
  `SPDK_URMA_BONDING_MULTIPATH_ENABLE=0` 时，设备为 BALANCE+PORT，且 initiator/target
  创建的 JFS 均开启 `multi_path=1`，与 TE 的 multipath=on 一致。原有
  `SPDK_URMA_BONDING_MULTIPATH_ENABLE=1` 仍选择 IODIE，不需要为 PORT 将它改成 1。
  raw 默认不启用该位；原有显式 multipath 开关的行为保留。

configure 自动编译、链接探测四个 get/put API，内部宏为 `SPDK_CONFIG_URMA_RJETTY`
和 `SPDK_CONFIG_URMA_SEG_CTX`，无需新增构建开关。缺少接口的 SDK 仍可编译 raw 路径；
bonding 在打开设备时明确返回 `-ENOTSUP`，不静默回退到旧的内核元数据交换路径。
同一程序必须使用 ABI 配套的 UMDK 头文件、core、provider 和内核驱动。

**wire version 为 6。target、urma_perf、Mooncake master/客户端依赖的 SPDK 均须重编
并同步更新；不支持混用 v4/v5/v6。** rjetty context 上限 16 KiB，segment context
上限 4 KiB，每个 endpoint 最多 32 个 jetty；非法长度在导入或复制前拒绝。

capsule 只发送实际使用的 context 长度，raw 不发送冗余 blob。较小 command 继续 inline
SEND；较大的 bonding command 使用按 qpair 延迟分配、一次注册的 TX slot 池。slot
由 SEND completion 释放，独立于 NVMe response 和 CID 复用；断连须等待完成排空。
target RX slot 与请求槽各预留最多 4 KiB context，内存开销随队列深度增长。

光互联显式选路需 `SPDK_URMA_NUMA_AFFINITY_ENABLE=1`，以及
`SPDK_URMA_ELECTRICAL_NETWORK_ENABLE=false`。后者也可从现有
`MC_UB_ELECTRICAL_NETWORK_ENABLE` 读取，默认 false。显式设置 NUMA affinity 为 0
仍保持 provider 默认选路；之前为规避 remote/remote 实现而设置的 0 不会被代码覆盖。

不依赖 URMA 硬件的回归：

~~~bash
python3 test/urma/handshake_build.py --umdk /path/to/UMDK --sanitize
python3 test/urma/registration_build.py --umdk /path/to/gpu-capable/UMDK
~~~

测试编译完整 common/initiator/target 源码，并运行真实 jetty 构造、HELLO、capsule 解码、target
数据 WR 和 initiator SEND 完成路径。覆盖完整扩展、缓存身份/保活、光/电选路、raw、
分段与背压、buffer 重投、跨 qpair 完成、发送失败和断连。provider fixture 不能替代
实际 aarch64 库与集群上的 bonding Admin Connect、读写和 master 挂载验证。

### 运行参数

默认值参考 Mooncake 当前 URMA 实现：RM 模式、每块本地 NIC 共享 2 个发送 JFC 和 2 个接收 JFC、每个远端 NIC endpoint 1 个 Jetty、JFC 深度 4096、Jetty 深度 2048、max_sge 5、rnr_retry 7、err_timeout 17、token `0xACFE`，active port 默认自动选择。priority 默认根据设备 `priority_info` 自动选择 CTP 对应项。

SPDK 使用以下环境变量。有两个 Mooncake 变量名作为低优先级回退被读取：`SPDK_URMA_ELECTRICAL_NETWORK_ENABLE` 未设置时读取 `MC_UB_ELECTRICAL_NETWORK_ENABLE`；`SPDK_URMA_LOG_REDIRECT` 未设置时读取 `MC_URMA_LOG_REDIRECT`（日志 sink 是进程级单例，与同进程的 Mooncake TE 共用一个开关）。其余 `MC_URMA_*` / `MC_UB_*` 变量均不被代码读取，按 Mooncake 习惯只设置它们会静默落回 SPDK 默认值：

| SPDK 参数 | Mooncake 回退变量 | 默认值 |
|---|---|---|
| `SPDK_URMA_TRANS_MODE` | 无 | `RM` |
| `SPDK_URMA_ACTIVE_PORT` | 无 | 自动选择 active port |
| `SPDK_URMA_BONDING_BALANCE` | 无 | `false` |
| `SPDK_URMA_BONDING_MULTIPATH_ENABLE` | 无 | `false` |
| `SPDK_URMA_ELECTRICAL_NETWORK_ENABLE` | `MC_UB_ELECTRICAL_NETWORK_ENABLE` | `false` |
| `SPDK_URMA_DEV_NAME` | 无 | 第一个匹配设备 |
| `SPDK_URMA_EID_INDEX` | 无 | `0`，不存在时使用首个 EID |
| `SPDK_URMA_SEND_JFC_COUNT` | 无 | `2`；`SPDK_URMA_JFC_COUNT` 为兼容别名 |
| `SPDK_URMA_RECV_JFC_COUNT` | 无 | `2`；`SPDK_URMA_JFC_COUNT` 为兼容别名 |
| `SPDK_URMA_JFC_DEPTH` | 无 | `4096` |
| `SPDK_URMA_NUM_JETTY_PER_EP` | 无 | `1`；`SPDK_URMA_JETTY_COUNT` 为兼容别名 |
| `SPDK_URMA_JETTY_DEPTH` | 无 | `2048` |
| `SPDK_URMA_MAX_IO_SIZE` | 无 | `131072` |
| `SPDK_URMA_CAPSULE_TRANSPORT` | 无 | `tcp`；可设为 `urma`（代码只接受 `tcp` / `urma`，旧文档写的 `sendrecv` 不是合法值） |
| `SPDK_URMA_TP_TYPE` | 无 | `ctp`；可设为 `rtp` |
| `SPDK_URMA_JETTY_PRIORITY` | 无 | 自动从 `priority_info` 选择；显式值范围 `0..15` 且必须匹配 TP type |
| `SPDK_URMA_NUMA_AFFINITY_ENABLE` | 无 | `false` |
| `SPDK_URMA_LOG_REDIRECT` | `MC_URMA_LOG_REDIRECT` | 开启（`0/off/false/no` 关闭）；需 SDK 提供 `urma_register_log_func`，否则编译期降级为不注册 |

**provider 日志重定向**：liburma 的 `urma_log()` 在未注册输出回调时丢弃全部消息，`URMA_LOG_LEVEL` 只控制过滤级别，单独设置它不会让 provider 日志落盘。当所选 SDK 声明了 `urma_register_log_func`（`configure` 探测后生成 `SPDK_CONFIG_URMA_LOG_FUNC`）时，SPDK 在 `urma_init()` **之前**通过 `pthread_once` 注册一次 sink，把 URMA/UDMA/UMMU/bondp 的日志按 syslog 级别映射到 `spdk_log()`：0–3→ERROR、4→WARN、5→NOTICE、6→INFO、7+→DEBUG。注册早于 init 是必要的，provider 驱动在 init 阶段才被打开并继承该回调。日志量由 `URMA_LOG_LEVEL` 与 SPDK 自身 log level 共同约束；设 `SPDK_URMA_LOG_REDIRECT=0` 可完全关闭。回调是进程级 last-wins，同进程 Mooncake TE 的 `UrmaRegisterLogRedirectIfEnabled()` 也注册自己的 sink，两者写入的流都会被 `kubectl logs` 捕获，任一生效均可；SPDK 不在 teardown 时注销，以保留退出阶段的驱动日志。

**与 Mooncake TE 的策略口径差异（部署前必读）**：

- `SPDK_URMA_BONDING_MULTIPATH_ENABLE=1` 选择 `BALANCE+IODIE`；Mooncake TE 的 `MC_URMA_BONDING_MULTIPATH_ENABLE=on` 选择 `BALANCE+PORT`。**同名开关语义相反**，不要照搬 Mooncake 的取值。需要 `BALANCE+PORT` 时用 `SPDK_URMA_BONDING_BALANCE=1` + `SPDK_URMA_BONDING_MULTIPATH_ENABLE=0`（见上文 wire v6 段）。
- SPDK 的 `electrical_network` 默认 `false`（光互联），Mooncake TE 的 `ub_electrical_network` 默认 `true`（电互联）。`numa_affinity` 打开时，前者 WR 选路为 `{local,remote}`、后者为 `{remote,remote}`；两端应显式配置，不要依赖默认值一致。
- SPDK 只在 `bonding_balance || bonding_multipath` 时下发 `SET_BONDING_MODE` 且 mode 恒 `BALANCE`；Mooncake TE 对 bonding 设备无条件下发（含 `STANDALONE/PORT`）。裸设备缺省模式下发是有意规避 jetty UAF，属有意分歧。

NVMf target 的 transport-specific JSON 还可覆盖 `dev_name`、`trans_mode`、`capsule_transport`、`active_port`、`eid_index`、`send_jfc_count`、`recv_jfc_count`、`jfc_depth`、`num_jetty_per_ep`、`jetty_depth`、`priority`、`tp_type`、`bonding_balance`、`bonding_multipath` 和 `numa_affinity`。`capsule_transport` 接受 `tcp` 或 `urma`（`spdk_urma_parse_capsule_transport()` 只认这两个值，`sendrecv` 会被拒为 `-EINVAL`），`tp_type` 接受 `ctp` 或 `rtp`。initiator 与 target 必须配置成相同 capsule、transport、TP type 和 endpoint Jetty 数；握手发现不一致时会拒绝连接。

同一进程内每块本地 NIC 只创建一个 URMA context。该 context 的 JFC/JFR、内存域和 target iobuf 整池注册由所有 endpoint/qpair 共享；target 的远端 segment import cache 是 context 级分桶哈希表，并以 `URMA_NON_CACHEABLE` 导入。`bonding_balance`（bonding 设备）或 `bonding_multipath` 任一开启时，initiator/target 创建的 JFS 均开启 `multi_path=1`；`SPDK_URMA_BONDING_MULTIPATH_ENABLE=1` 另将设备 bonding level 切到 IODIE，BALANCE+PORT 则保持 PORT。再启用 `numa_affinity` 时，数据 WR 根据 initiator buffer 的 NUMA 节点携带 chip affinity。NUMA 到 chip 的映射优先读取 sysfs `physical_package_id` 并按 package 排序生成 1-based chip ID，受限环境无法读取时才回退到节点前后半区启发式。

`tcp` 保持原有 capsule 数据路径，也是默认值。`urma` 只把 **命令/响应 capsule 的 SEND/RECV** 切到 URMA：建连握手仍走 TCP bootstrap socket，I/O payload **仍走 URMA READ/WRITE**，继续使用 jetty/JFS 和 JFC。因此「TCP capsule 完全绕开 JFR/JFC」的说法不准确——绕开的只是命令/响应方向的 URMA SEND/RECV，数据搬运和它的完成队列并未绕开（`lib/nvmf/urma.c` 的 `nvmf_urma_post_data()`）。SEND 使用 inline WQE（bonding 设备改用注册 TX 池槽位，其生命周期延伸到 SEND completion）；设备的 `max_jfs_inline_len` 不足以容纳 capsule frame 时，qpair 创建会失败。接收端按协商后的队列深度预投递 receive WR，并在消费 completion 后先补回 receive WR 再交付 NVMe completion。

#### HELLO_RSP 即 per-qpair 双向 RX 就绪门（wire v6，2026-11-21）

现场 `status=10` = `URMA_CR_RNR_RETRY_CNT_EXC_ERR`（远端 JFR 无 receive buffer）的根因是顺序缺口：target 原先先发完 HELLO_RSP，随后才由 `spdk_nvmf_tgt_new_qpair()` 异步派发 `poll_group_add()` → `capsule_resources_init()` 投递 receive WR。initiator 一看到 HELLO_RSP 就可能发 Connect，此时 target 的 JFR 还是空的。

现在 target 在 `handshake_apply()` **成功之后、写出第一个 HELLO_RSP 字节之前**调用 `nvmf_urma_handshake_rx_barrier()` 投递本 qpair 的命令 receive WR。因为 initiator 必须读完完整 HELLO_RSP 才发 Connect，两个方向由此得到严格保证：

| 方向 | 就绪点 | 保证 |
|---|---|---|
| initiator → target（Connect/cmd capsule） | target 在 HELLO_RSP 之前投递命令 receive WR | initiator 读到 HELLO_RSP 时 WR 已就位 |
| target → initiator（response capsule） | initiator 在 `exchange_hello()` 之后、`connect_async()` 之前由 `nvme_urma_capsule_resources_init()` 投递响应 receive WR | target 收到第一条命令时 WR 已就位 |

`capsule_resources_init()` 对同一 qpair 幂等（已投递则直接返回），所以异步的 `poll_group_add()` 不会重复投递 WR、也不会重复持有 lifetime 引用。若 HELLO_RSP 写出中途失败，握手拒绝路径走 `qpair_fini()` 的排空状态机，等 flush CR 落完再释放 slot，避免复用已释放内存。

该修复**不 bump wire version**（仍是 6），不新增 READY 消息，不改 `share_jfr=1`：每个 qpair 仍持有自己创建、交给 jetty 使用的 JFR，`share_jfr=1` 只表示使用外部创建的 JFR，不能推断成「所有 qpair 共用一个接收队列」。TCP capsule 模式在该 helper 内是 no-op，其数据路径仍经 URMA READ/WRITE + JFC。回归见 `test/urma/jetty_handshake.c` 的 `roundtrip()` 与 `rx_barrier_failure()`：断言 barrier 恰好调用一次、发生在 RECV_HELLO 状态且 HELLO_RSP 尚未写出，并断言 barrier 失败时握手在发出任何响应字节之前中止。

#### Admin 请求端到端关联诊断

`cmd_received` / `response_post` 原先只在 `opcode == SPDK_NVME_OPC_FABRIC` 时打印，Identify 等 Admin 命令是盲区。现在由 `SPDK_URMA_DIAG_ADMIN()` 统一门控（Admin qid、`0x7f` Fabrics、或显式开启 stage trace 的 I/O），避免每条 I/O 都刷 NOTICE。同一条请求可在两端按 `qid/cid/opcode/fctype/trace_seq` 串起来：

```
initiator cmd_submitted(rc) → target cmd_received → target data_posted(READ/WRITE)
→ target data_completed → target response_post(rc,sct,sc)
→ initiator rsp_received → initiator rsp_matched
```

注意 `response_post rc=0` 只证明字节写入了 socket（或 SEND WR 提交成功），**不证明 master 收到、解析并匹配**；接收与匹配分别由 `rsp_received` / `rsp_matched` 提供正向证据，`nvme_urma_complete_response()` 里「no matching ureq for cid」仍是匹配失败的负向证据。

握手描述符现在逐 jetty 携带 `urma_get_rjetty()` 返回的完整 provider context，command capsule 另带完整 `urma_get_seg_ctx()` context，因此 wire version 为 **6**（v4/v5/v6 不可混用，两端须同步重编）；新旧版本会在握手阶段明确拒绝，而不会继续使用不一致的帧布局。上述 RX 就绪门只改变 HELLO_RSP 与 receive WR 投递的相对顺序，不改变任何线上字节，因此**不需要** bump 到 v7。

### 最小联调示例

target 侧在创建 bdev 与 subsystem 后创建 URMA transport 和 listener：

~~~bash
scripts/rpc.py nvmf_create_transport -t URMA
scripts/rpc.py nvmf_subsystem_add_listener nqn.2016-06.io.spdk:cnode1 \
    -t URMA -a 0.0.0.0 -s 4420
~~~

启用 URMA SEND/RECV capsule 时，target 与 initiator 分别配置：

~~~bash
scripts/rpc.py nvmf_create_transport -t URMA --capsule-transport urma
export SPDK_URMA_CAPSULE_TRANSPORT=urma
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

每个进程按本地 URMA NIC 和兼容配置创建一个共享 URMA context；同一 NIC 的 endpoint/qpair 复用该 context 的发送 JFC、接收 JFC、JFR 和 memory domain。每个 endpoint 持有可配置数量的 Jetty，并以 round-robin 方式选择一个发送 JFC；target poll group 按 worker 编号分片轮询 context 的 JFC。传输模式支持配置为 RC、RM 或 UM，双方必须一致。

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
8. **bond 线失败层次未定**：doc32 的「target TCP send 成功 → ubagg 回程断链」证据不足，需按 §14.1 的单变量矩阵重跑后再决定是否改 provider。

### 14.1 bond 数据 WR / NUMA affinity / 完成队列归属的单变量验证（待执行）

审阅指出 v3 在选 bond 时同时打开了 `BALANCE` 和 `numa_affinity`，raw 则关闭——两个变量一起变，无法把差异归因到任何一个。以下是**待执行**的验证矩阵，只做只读观测与配置切换，**不改 provider、不改 `share_jfr`**（`share_jfr=1` 表示使用外部创建的 per-qpair JFR，不能推断成「所有 qpair 共用一个接收队列」，也不应改成 0）。

固定不变量（每轮前后都记录并核对）：设备名与 `urma_dev`、`rm|rc`、`capsule_transport`、`bonding_balance`/`bonding_multipath` 的 bonding level、`jetty_count`、`jfc_depth`、`rnr_retry`、镜像 commit（两端一致）、`SPDK_URMA_*` 全量 env 快照。

| 轮 | 唯一改变量 | 固定值 | 采集 |
|---|---|---|---|
| A | 基线：`numa_affinity=0`，`electrical_network=0`，无 BALANCE | raw 同样参数 | 成功/失败计数、`cr_error status` |
| B | 仅 `SPDK_URMA_NUMA_AFFINITY_ENABLE=1` | 其余同 A（bond/BALANCE/PORT 不动） | 同上 + `data_posted/data_completed` 是否成对、`src_chip/dst_chip` |
| C | 仅 `SPDK_URMA_ELECTRICAL_NETWORK_ENABLE=1` | 其余同 A | 同上 + chip 选路 `{remote,remote}` vs `{local,remote}` |
| D | 仅 `SPDK_URMA_BONDING_BALANCE=1` | 其余同 A，`numa_affinity=0` | 同上 + JFS `multi_path` 位 |
| E | B+D 组合（复现 v3 现场） | — | 与 B/D 单变量结果对照，确认是否出现非线性交互 |

每轮必须同时采集两端的 `cmd_submitted` / `cmd_received` / `data_posted` / `data_completed` / `response_post` / `rsp_received` / `rsp_matched`（§14 Admin 关联诊断），并按 `qid/cid/opcode/fctype/trace_seq` 对齐；另需记录发送 JFC 与接收 JFC 的归属（`send_jfc_index`、poll group 的 `worker_index` 分片）以排除「CR 落在无人轮询的队列」这一独立变量。判定顺序：

1. 若 `cmd_submitted rc != 0` 或缺 `cmd_received` → 问题在提交/胶囊投递，与 bond 数据 WR 无关。
2. 若有 `data_posted` 无 `data_completed` → 问题在数据 WR / JFC 完成，检查完成队列归属与 flush CR。
3. 若 `response_post rc=0` 但缺 `rsp_received` → 才是回程投递问题，此时再用 tcpdump 抓包确认字节是否到达网卡。
4. **只有**当上述链条完整、且仅 B/D/E 出现差异时，才把差异归因到 NUMA affinity / BALANCE，并考虑 provider 修改。

注意本矩阵不含 `urma_perf` 长稳压测；`examples/nvme/urma_perf/urma_perf.c` 的 drain 循环目前只在 `g_failed` 为真时启用超时，completion 丢失但 `g_failed=false` 时 `run_workers()` 不会返回（`print_results()` 不打印）。压测前需单独决定是否改成无条件有界超时，避免把「卡住」误判成底层断链。

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

## 17. Embedded initiator Admin registration diagnostics (2026-11-22)

A target receiving Connect/Property Get capsules does not prove that controller
initialization or the following Identify transfer succeeded. In particular,
`nvme_ctrlr_identify()` changes the state to ERROR when its submission returns an
error. A later Property Get during shutdown is not the original failure.

`nvme_urma_qpair_submit_request()` now reports every post-allocation submission
failure at ERROR level, with endpoint, device, qid, cid, opcode, length and stage:

- `memory_register`: preparing the local payload region failed; no command
  capsule has been sent for that request.
- `segment_context`: exporting the complete bonding region context failed;
  this request has not been sent either.
- `capsule_send`: registration/context preparation succeeded but capsule
  submission failed. The peer has not necessarily received a complete frame.

The common memory registration code clears errno before a provider call and
captures it immediately on failure. A provider returning NULL without setting
errno produces `-EIO`, not a stale `-ENOENT` from an earlier syscall. Its
`role=memory stage=register_failed` record includes the original range, aligned
grant range and provider errno. This corrects error reporting; it does not make
an unsuccessful provider registration succeed or establish a fabric root cause.

Tests (no URMA hardware required):

```bash
python3 test/urma/admin_submit_build.py --umdk /path/to/UMDK --sanitize
python3 test/urma/registration_build.py --umdk /path/to/UMDK
python3 test/urma/handshake_build.py --umdk /path/to/UMDK --sanitize
```

The Identify fixture runs the real submission/cache/CID code over a local TCP
socketpair. It verifies ERROR-level `-ENOENT` correlation, no partial send or CID
leak, retry after a failed registration, cache reuse, bonding context export,
raw TCP submission and export failure cleanup. The registration fixture checks
stale errno independently of provider behavior. These tests do not emulate the
ARM bonding provider and cannot confirm that the reported cluster failure is
fixed. Rebuild SPDK's initiator library and relink Mooncake master/kvtest before
collecting the next attach attempt; replacing `nvmf_tgt` alone does not update
statically linked initiators.

## 18. Request-owned bonding Admin payload registrations (2026-11-23)

For bonding devices, Admin queue payloads that are not covered by an
application-owned registry region no longer enter the qpair registration cache.
They remain registered while the request is outstanding and follow the existing
uncached-region cleanup on completion, submit failure or abort, before callbacks
may free/reuse the DMA buffer. Raw devices and I/O queue caching retain their
existing policy. Registry regions keep their original application ownership.

This prevents a completed 1KB Connect buffer from leaving a cached 4KB grant
which a later 4KB Identify at the same address tries to register again: the
cache keys were the original `(va, payload_length)`, while the provider grants
page-aligned ranges. Bond providers can reuse a physical TID and reject that
second token-NONE grant. This change handles the transient Admin lifecycle;
it is not a general solution for concurrent overlapping I/O registrations.

`URMA_DIAG stage=admin_region_ready` and `stage=admin_region_release` record
context, qid/cid/opcode/fctype, original range, actual grant range, region,
segment token ID and external/cached ownership. They are limited to bonding
Admin payloads and written to stderr so embedded masters filtering SPDK NOTICE
still capture them. They preserve errno. `segment_token_id` belongs to the
exported segment, not necessarily the underlying physical pctx TID. A release
record precedes the provider unregister call; it is not proof that an ungrant
succeeded. Provider error logs remain necessary if unregister fails.

`test/urma/admin_submit_build.py --umdk /path/to/UMDK --sanitize` now exercises
the production submit, response completion and abort paths. A provider fixture
rejects duplicate grants for the same context/range: before the fix, completed
Connect followed by same-address Identify returns `-EINVAL`; after it, the old
grant is released before the callback and the Identify succeeds. It also checks
error completion, send/export failures, abort, external region ownership, raw
Admin cache reuse and bonding I/O cache reuse. This models the duplicate-grant
contract; it does not emulate the ARM provider or establish which earlier grant
occupied the range in the recorded cluster. Rebuild the initiator library and
relink the Mooncake master before validating the next attach.
