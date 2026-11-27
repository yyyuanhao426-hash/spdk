# URMA iobuf slot 与命令上限独立配置

2026-11-26 实现于 `urma_enable_v3`；2026-11-27 同步到 `urma_enable_v2`。
两条分支共用此 iobuf/URMA 能力，readcache 层仅在 v3。

## 配置语义

SPDK 不知道 Mooncake 的 `MC_NOF_SUBMIT_CHUNK_BYTES`。chunk 是上层提交策略，
RAID strip 是后端拆分边界，两者都不自动成为到达 target 的单条请求大小。
需要由调用方把期望的 slot 字节数通过已有 SPDK 接口明确传入：

| 入口 | 控制内容 |
|---|---|
| startup RPC `iobuf_set_options --large-bufsize` | 全局 iobuf large slot 的实际字节数 |
| takeover `-I small_count,small_size,large_count,large_size` | 同一 RPC 的池数量及 slot 尺寸 |
| `nvmf_create_transport -i` / takeover `-O` | URMA 命令大小的配置上限 |
| initiator `SPDK_URMA_MAX_IO_SIZE` | initiator 自身传输上限，仍参与取 min |

本次不新增 Mooncake 参数，不从 RAID 条带推测请求大小，不改变全局 iobuf 默认值。
原生 SPDK 默认 large slot 是 135168 字节；现有 NoF profile 的 8 MiB 来自显式 `-I`。
只保留 `-O=8 MiB`、不配置池，并不会自动分配 8 MiB slot。

URMA 单 buffer 数据路径现在按实际 iobuf 配置计算能力：

```text
slot_capacity = max(small_bufsize, large_bufsize)
effective_max_io_size = floor_power_of_two(min(configured_max_io_size, slot_capacity))
qpair_max_io_size = min(effective_max_io_size, initiator_max_io_size)
```

通常 large 比 small 大，slot_capacity 就是 large_bufsize。向下取 2 的幂是为了
保持 transport 命令上限约定，并兼容原生 132 KiB 等非 2 次幂的池尺寸。
不足 8 KiB 的配置不能创建 URMA transport。

HELLO 回复携带实际协商上限，initiator 的现有 NVMe 通用提交路径据此拆分大 I/O。
直接绕过该路径的调用方也必须遵守协商值。忽略协商值而向 target 发送超限单条
命令，仍返回 `-EMSGSIZE`；本次没有添加多 iovec URMA 传输。

## 裸 SPDK 与 takeover 用法

以下是新编译 runtime 的配置示例，由用户按现有启动流程使用：

```bash
# nvmf_tgt 以 --wait-for-rpc 启动，尚未 framework_start_init 时设置池。
scripts/rpc.py iobuf_set_options \
  --small-pool-count 16384 --small-bufsize 8192 \
  --large-pool-count 1024 --large-bufsize 262144
scripts/rpc.py framework_start_init
scripts/rpc.py nvmf_create_transport -t URMA -i 8388608 \
  --capsule-transport tcp --iobuf-large-cache-size 32 --iobuf-small-cache-size 1024
scripts/rpc.py nvmf_get_transports
```

同一参数通过裸机脚本传入：

```bash
# 将 -I 第四项独立设置为选定的 chunk 粒度，例如 256 KiB。
# 其余参数沿用自己的已审阅接管命令；此示例不由 agent 执行。
./target_nvme_takeover.sh -d <空闲盘> \
  -I 16384,8192,1024,262144 -O 8388608 -C 32
```

如果外部选定 128 KiB 或 512 KiB，分别传入 `131072` 或 `524288`。
slot 仍为 8 MiB 的配置保持原行为。`-I` 单独使用时，也会显式设置 PG iobuf 缓存，
不再依赖是否同时传入 `-O`。

`nvmf_get_transports` 保留配置的 `max_io_size`，新增状态：

```json
{
  "max_io_size": 8388608,
  "effective_max_io_size": 262144,
  "iobuf_small_bufsize": 8192,
  "iobuf_large_bufsize": 262144
}
```

前者仍用于保存配置，后者说明 URMA 实际提供的能力；若 initiator 上限更小，
该 qpair 的协商值还会进一步下降。startup 日志同时记录请求上限、池尺寸和能力上限。
池尺寸在 startup 设置，不能把本次变更理解为在线 resize。

1024 个 large slot 从 8 MiB 改为 256 KiB，large 池请求内存从 8 GiB 变为 256 MiB。
small 池、large 数量及 `-C` 不随尺寸改变。只有实际 child 原本就不超过新上限时，
单个业务 child 占用 slot 数才保持不变；更大的命令会拆成更多 child，须测量开销。
普通分配器开销、SPDK 其他内存、dirty DMA 预算和 Pod/宿主机 hugepage 额度需另外核算。
takeover 原有 hugepage 最低值和资源预算公式保留，不保证把节省的池内存自动归还给系统。

## 本地验证与边界

```bash
python3 test/urma/iobuf_limits_build.py --umdk /home/senorita/GDR/GDR_Related/UMDK_netlab
python3 test/urma/iobuf_limits_build.py --umdk /home/senorita/GDR/GDR_Related/UMDK_netlab --sanitize
python3 test/urma/handshake_build.py --umdk /home/senorita/GDR/GDR_Related/UMDK_netlab
python3 test/readcache/run.py
```

专项通过真实 URMA create/dump、SPDK JSON writer 和通用 request iobuf 获取/等待回调，
验证 8 MiB 配置上限配 8 KiB/132 KiB/256 KiB/384 KiB/512 KiB/8 MiB slot 的能力选择、
保留池数量、恰好一个 buffer、暂缺 buffer 后回调重试、RPC 状态及裸机脚本参数。
新增真实两端 HELLO 回归覆盖 requested=8 MiB、effective=256/512 KiB，以及 bonding
RM/RC 和 legacy raw SDK；原完整 URMA transport 源码 SDK 矩阵编译、readcache/W2 回归保留。
这些使用本地模拟设备/池，不是实际运行中 SPDK JSON-RPC socket、SSD 或 URMA 性能验收。

原分析记录位于 `~/nof/docs/spdk/iobuf/2026-11-26-iobuf-large-slot-8mib-optimization.md`。
该文旧版本的“必须成对调小 -I/-O”“大于 slot 必然到 target 后多 iovec 失败”是旧路径限制；
本次将安全能力协商放进 URMA 本身，裸 SPDK RPC/C 调用也生效。slot 始终由外部配置，
不能把协商上限下降误认为池内存自动缩小。真实吞吐/P99、retry 和 split 数仍待实测。
