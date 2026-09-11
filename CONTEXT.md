# NOF/URMA GPU 存储性能测试

基于 SPDK fork（urma_modified_v3 分支）的 URMA 传输 + GPU 显存路线验证：把远端 NVMe
经 NOF/URMA 暴露给 GPU 节点，用 urma_perf 对比不同内存路线（GDR 直达 vs 分级拷贝 vs 纯
CPU）的带宽/延迟，并用两侧分阶段打点定位瓶颈。部署与操作细节见 docs/spdk_urma_deploment.md
（不在本仓库内），本文件只收录这个域的语言。

## Language

### 角色与拓扑

**Initiator**:
发起 NVMe/URMA I/O 的节点（151，带 Tesla V100，运行 urma_perf）。
_Avoid_: 客户端、host（与"host 内存"歧义）

**Target**:
持有一块 NVMe 盘并跑 nvmf_tgt 提供服务的节点（245 及后续多台部署机）。
_Avoid_: 服务端、存储节点

**接管（takeover）**:
把 NVMe 盘从 nvme 驱动 unbind、bind 到 vfio-pci（noiommu 模式），供 nvmf_tgt 用户态直通。
接管对象只能是空闲盘。

**系统盘（禁碰盘）**:
有挂载、swap、LVM PV 或 md 成员身份的 NVMe 盘；其 BDF 永不参与接管。
_Avoid_: 根盘（不止根分区所在盘）

**残留盘**:
上次运行失败/中断后，留在 vfio-pci 占用或无驱动状态的 NVMe 控制器；每次接管前必须先还原回 nvme 驱动。
_Avoid_: 脏盘、遗留设备

**URMA 设备名**:
两端必须显式统一（`SPDK_URMA_DEV_NAME`，如 udmac0d1e2）的 URMA 设备标识；重启后枚举可能重排。
_Avoid_: 网卡名、dev 名

### 内存路线（urma_perf 的 `-M`，四选一）

**内存路线（mem-type）**:
urma_perf 中 I/O 缓冲区的产地与向 URMA 注册的方式。对比实验的单位。

**posix**:
分级路线——数据产自 HBM，每个 I/O 先经分级拷贝落到 host 暂存缓冲，暂存缓冲作为 URMA
segment 注册。模拟无 GDR 时 "HBM → CPU 拷贝 → NIC DMA" 的真实路径。
_Avoid_: 纯 host 内存、无 GPU 路径（旧义，已改名为 cpu）

**cpu**:
纯 host 大页内存路线，不碰 CUDA，缓冲区直接注册（2026-09 改版前 posix 的行为）。
_Avoid_: posix（不要再用 posix 指代此路线）

**peermem**:
GDR 路线——HBM 经 nvidia_p2p peer-memory 直接注册给 NIC，无拷贝。
_Avoid_: p2p、GDR 模式（GDR 是统称，见下）

**dmabuf**:
HBM 经 cuMemGetHandleForAddressRange 导出 dmabuf 后注册的路线（urma_perf 默认）。

**GDR（GPU Direct RDMA）**:
NIC 直接 DMA GPU 显存这一类路线的统称（peermem 与 dmabuf 都是 GDR 的实现）。
_Avoid_: 直通（与 vfio 直通歧义）

**暂存缓冲（staging buffer）**:
posix 路线中 4K 对齐的 host 缓冲，是唯一被 URMA 注册的缓冲。

**影子缓冲（shadow buffer）**:
posix 路线中 cuMemAlloc 出来的 HBM 缓冲，数据的"产地"，不直接参与传输。

**分级拷贝（staged copy）**:
posix 路线中每个 I/O 的 HBM↔host 一次 cuMemcpy（写方向提交前 DtoH，读方向完成后
HtoD）；其耗时单独统计，不计入 NVMe 延迟——端到端成本 = 延迟 + 拷贝。
_Avoid_: 预拷贝（不是只做一次，是每 I/O）

### 数据面与吞吐

**拉数（pull）**:
URMA 写方向的数据搬运：target 收到 capsule 后在 jetty 上发起远程读，把 initiator
缓冲的数据 DMA 进本地 iobuf。target 打点 "W8 JFC wait (pull)" 计的就是等它完成。
_Avoid_: 下载、反向读

**推送（push）**:
数据面的另一方向（C2H 直推），当前写路径的少数派；对应 target 打点
"push JFC wait (C2H)"。
_Avoid_: _

**拆分（split）**:
initiator 按 noiob（stripe 边界）把一条越界 I/O 拆成多个子请求的行为；strip 与
transport max_io_size 共同决定是否发生，拆分会成倍消耗 qpair request pool。
_Avoid_: 分片

**在飞窗口（window）**:
一端同时在网上的 I/O 数（urma_perf 为 -T×-b，经 qpair num_entries 与对端
max_queue_depth 协商收窄）。带宽 = 窗口 × I/O 大小 ÷ 往返。
_Avoid_: 队列深度（那是单连接的配置上限，不是实际在网数）

**每操作固定开销（per-op overhead）**:
与 I/O 大小无关、每次拉数都要付的控制面+建立成本（实测 ~60μs 量级），是 1MB I/O
聚合带宽 ~10.7 GiB/s 上限的来源；加大 I/O 是摊薄它的唯一杠杆，加盘/加连接无效。
_Avoid_: RTT（往返是端到端延迟、含排队，别与固定开销混用）

### 计时打点

**轮（round）**:
一次 urma_perf 进程的一次完整测量。initiator 侧天然按进程隔离；target 侧以"新连接进入"
为轮边界。
_Avoid_: 次跑、session

**按轮清零（per-round reset）**:
target 打点计数器在新连接进入时先 dump 上一轮最终累计、再清零的机制，使长驻 nvmf_tgt
的日志按轮独立成块。
_Avoid_: 自动清零、定时清零（不是按时间）

**打点（timing breakdown）**:
initiator/target 两侧 I/O 路径的分阶段耗时插桩（W4a parse capsule … W10 send response
等阶段编号），target 侧由 `SPDK_URMA_TARGET_DUMP_SEC` 控制打印周期。
_Avoid_: log、trace

### 测量纪律

**预检（preflight）**:
urma_perf 正式测量前的写→读回模式校验，覆盖所选内存路线的完整数据路径；预检不过则测试作废。
_Avoid_: 自检、验证（太泛）

**对比三线**:
同参数跑 `cpu` / `posix` / `peermem` 各一次的标准实验组：三者之差分别隔离出"GPU 参与 +
拷贝"与"拷贝本身"的开销。
