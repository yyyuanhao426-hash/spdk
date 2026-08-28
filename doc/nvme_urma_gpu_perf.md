# NVMe/URMA GPU 到远端 SSD 测试指南

本文说明如何编译和运行 SPDK 新增的 `urma_perf` 程序。该程序直接使用 CUDA
显存作为 NVMe I/O payload，通过独立的 NVMe/URMA initiator 连接远端 SPDK
NVMe-oF target，用来回答三个问题：

1. GPU 显存到远端 SSD 的 WRITE 通路是否可用；
2. 远端 SSD 到 GPU 显存的 READ 通路是否可用；
3. 指定 I/O 大小、线程数、batch size 和测试时间后的时延、IOPS 与带宽是多少。

> **数据破坏警告：**程序会从 `--start-lba` 开始覆盖目标 namespace。只能对专用
> 测试盘或允许销毁数据的 LBA 范围运行，不能对保存有效数据的盘运行。

## 1. 本次代码位置

- `examples/nvme/urma_perf/urma_perf.c`：测试程序；
- `examples/nvme/urma_perf/Makefile`：测试程序构建入口；
- `include/spdk/nvme_urma.h`：CUDA/NPU/XDS 等异构内存 provider 接口及注册统计；
- `lib/nvme/nvme_urma_common.c`：URMA segment 注册和统计实现。

UMDK 源码没有修改。SPDK 只把已经安装的 UMDK `liburma` 当作外部依赖；测试逻辑
和 CUDA 支持均位于 SPDK。

## 2. 实际数据路径

WRITE 测试的数据方向是：

~~~text
initiator CUDA buffer
  -> initiator 将显存注册为 URMA segment
  -> target 用 URMA READ 直接拉取数据
  -> target SPDK host buffer
  -> target bdev
  -> NVMe SSD
~~~

READ 测试的数据方向相反：

~~~text
NVMe SSD
  -> target bdev
  -> target SPDK host buffer
  -> target 用 URMA WRITE 写入 initiator CUDA buffer
~~~

这里消除了 initiator 侧的 HOST bounce buffer。当前 target 侧仍使用 SPDK host
buffer，因此本测试验证的是“GPU Direct + NVMe/URMA + 远端 SSD”链路，不是 target
侧 SSD 与 GPU 之间的 PCIe P2P。

正式计时前，线程 0 会执行一次数据正确性检查：生成 HOST pattern、复制到 GPU、
从 GPU WRITE 到 SSD、清空 GPU、从 SSD READ 回 GPU，最后复制回 HOST 并逐字节
比较。看到 `Preflight GPU WRITE + READ verification passed` 才表示双向数据通路通过。

## 3. 前置条件

target 与 initiator 都需要：

- 可用的 URMA kernel driver、设备、EID 和 UMDK/UVS 运行环境；
- 已编译安装的 UMDK URMA headers 与 `liburma`；
- 本分支 SPDK。

initiator 还需要 NVIDIA driver、支持的 CUDA GPU，以及 URMA provider 能直接访问
CUDA 显存的 peer-memory 或 DMA-BUF 能力。程序通过 `dlopen("libcuda.so.1")` 加载
CUDA Driver API，编译时不依赖 CUDA headers，也不链接 `libcudart`。

## 4. 编译

### 4.1 安装现有 UMDK

以下命令只编译、安装 UMDK，不修改其源码：

~~~bash
cd /path/to/UMDK_tool_netlab/src
mkdir -p build
cd build
cmake .. -DBUILD_ALL=disable -DBUILD_URMA=enable \
  -DCMAKE_INSTALL_PREFIX=/opt/umdk
make -j$(nproc)
sudo make install
~~~

如果环境已经用 RPM 或 yum 安装 `umdk-urma-lib` 与 `umdk-urma-devel`，可跳过此步。

### 4.2 编译 SPDK 与测试程序

UMDK 安装在 `/opt/umdk` 时：

~~~bash
cd /path/to/SPDK
git submodule update --init
./configure --with-urma=/opt/umdk
make -j$(nproc)
~~~

UMDK 安装在系统默认 include/library 路径时使用：

~~~bash
./configure --with-urma
make -j$(nproc)
~~~

构建完成后应存在：

~~~text
build/bin/nvmf_tgt
build/examples/urma_perf
~~~

可先确认参数解析和动态 CUDA loader：

~~~bash
sudo ./build/examples/urma_perf --help
ldd ./build/examples/urma_perf | grep -E 'urma|dl'
~~~

## 5. 配置远端 target

下面示例将 target 上 PCIe 地址 `0000:81:00.0` 的 SSD 作为 namespace 1 导出。
请替换 PCIe 地址、监听 IP 和 NQN。启动 target 后，在另一个终端执行 RPC：

~~~bash
cd /path/to/SPDK
sudo ./build/bin/nvmf_tgt -m 0x3
~~~

~~~bash
sudo scripts/rpc.py bdev_nvme_attach_controller \
  -b Nvme0 -t PCIe -a 0000:81:00.0

sudo scripts/rpc.py nvmf_create_transport -t URMA
sudo scripts/rpc.py nvmf_create_subsystem \
  nqn.2026-01.io.spdk:urma-gpu-test -a -s URMAGPU0001
sudo scripts/rpc.py nvmf_subsystem_add_ns \
  nqn.2026-01.io.spdk:urma-gpu-test Nvme0n1 -n 1
sudo scripts/rpc.py nvmf_subsystem_add_listener \
  nqn.2026-01.io.spdk:urma-gpu-test \
  -t URMA -f IPv4 -a 0.0.0.0 -s 4420
~~~

如果需要选择 URMA device/EID，可在启动 `nvmf_tgt` 前设置：

~~~bash
export SPDK_URMA_DEV_NAME=<urma-device-name>
export SPDK_URMA_EID_INDEX=0
~~~

还应使用环境现有的 `urma_admin`、UVS 或网卡工具确认两端设备与 EID 正常。具体
control-plane 命令依赖部署方式，不由 SPDK 测试程序创建。

## 6. 在 initiator 运行测试

### 6.1 WRITE：GPU 到远端 SSD

~~~bash
sudo ./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:192.0.2.10 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -w write -o 4096 -T 4 -b 32 -t 30 -n 1 -g 0 -l 0
~~~

### 6.2 READ：远端 SSD 到 GPU

~~~bash
sudo ./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:192.0.2.10 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -w read -o 131072 -T 4 -b 16 -t 30 -n 1 -g 0 -l 0
~~~

参数含义：

| 参数 | 含义 |
|---|---|
| `-o, --io-size` | 单个 NVMe I/O 字节数，必须按 namespace sector 对齐 |
| `-T, --threads` | worker 数；每个 worker 使用一个 SPDK core 和一个 I/O qpair |
| `-b, --batch-size` | 每个 worker 允许的最大 outstanding I/O 数 |
| `-t, --time` | 正式计时秒数，不包含 preflight 正确性检查 |
| `-w, --workload` | `read` 或 `write` |
| `-g, --gpu` | CUDA GPU ordinal |
| `-n, --nsid` | 目标 namespace ID |
| `-l, --start-lba` | 破坏性测试起始 LBA |
| `-m, --core-mask` | 可选 SPDK core mask；未给出时自动选择前 T 个 core |

加上 `--require-dmabuf` 后，只要 DMA-BUF 注册不可用或任一计时 I/O 使用了
peer-memory fallback，程序就以失败状态退出：

~~~bash
sudo ./build/examples/urma_perf ... --require-dmabuf
~~~

不加该选项时，DMA-BUF 不可用会尝试当前 UMDK 支持的 GPU peer-memory 注册。

## 7. 如何判断结果

典型输出包含：

~~~text
Preflight GPU WRITE + READ verification passed at LBA 0
thread=0 core=0 completed=... errors=0
NVMe/URMA GPU -> remote SSD result
operation=write io_size=4096 threads=4 batch_size=32 elapsed=30.... s
completed=... bandwidth=... MiB/s IOPS=...
latency_us avg=... min=... p50=... p99=... p99.9=... max=...
memory_registration accelerator=... dmabuf=... peer_memory_fallback=... failures=0
~~~

验收条件建议同时满足：

1. preflight 双向数据比较通过；
2. 所有线程 `errors=0`，程序退出码为 0；
3. `accelerator` 大于 0，证明提交的是 accelerator memory；
4. 要求 DMA-BUF 时，`dmabuf` 大于 0、`peer_memory_fallback=0`；
5. `bandwidth`、`IOPS` 和所需时延百分位达到目标。

时延从 SPDK 提交单个 NVMe 命令前开始，到该命令 completion callback 为止；在当前
原型中还包含每请求 URMA memory register/unregister 的成本。带宽按所有 worker 的
成功完成字节数除以实际运行时间计算。batch size 表示每线程队列深度，因此总最大
outstanding I/O 约等于 `threads * batch-size`。

## 8. 建议的验证顺序

1. 先用 `-T 1 -b 1 -o 4096 -t 5` 验证连接和数据正确性；
2. 分别跑一次 WRITE 和 READ；
3. 加 `--require-dmabuf` 确认 DMA-BUF 路径，或从统计确认 peer-memory 路径；
4. 再逐步增加 batch size 和线程数；
5. 用 4 KiB、16 KiB、64 KiB、128 KiB 分别测试，并保存 p99 和带宽；
6. 与 target 本地 SPDK `perf` 结果对照，分离 SSD 上限和网络/GDR 开销。

若 preflight 失败，应依次检查 CUDA driver、GPU 显存导出、URMA memory registration、
两端 EID/Jetty 建连、target namespace 和 SSD I/O，而不要使用性能数字判断通路正确性。
