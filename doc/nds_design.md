# NDS（NPU Direct Storage）设计说明

> NDS 是 GDS（GPU Direct Storage，同事B基于 SPDK + URMA 实现）的 NPU 版本：
> 昇腾 NPU 的 HBM 经 URMA 总线直连远端 NVMe 盘，绕过 host 内存中转。
> 本文档记录 Phase 1（SPDK 层）的实现，以及 Phase 2（UMDK / 内核驱动层）
> 的方案要点和待验证问题。

## 1. 三层架构

```
┌─ SPDK 层（本仓库，Phase 1 已实现）────────────────────┐
│ urma_perf -M npu / npu-staged                          │
│ urma_perf_npu.c：dlopen libascendcl.so（AscendCL），   │
│   NPU memory provider（type=SPDK_NVME_URMA_MEM_NPU）   │
└──────────────────┬─────────────────────────────────────┘
                   │ seg_cfg.is_gpu_seg = 1（udrv_data 私有通道）
┌─ UMDK 用户态 liburma（Phase 2）────────────────────────┐
│ udma_u_register_seg()：把 GPU/NPU 标志传给内核驱动      │
└──────────────────┬─────────────────────────────────────┘
┌─ 内核驱动 urma_driver（Phase 2）───────────────────────┐
│ udma_gpu_p2p.c：函数指针 provider 框架                 │
│   udma_register_gpu_p2p_ops()（GPL 导出符号）          │
│ udma_nv_p2p_bridge.ko：现有 NVIDIA 桥接实现            │
│ udma_npu_bridge.ko（待做）：NPU 桥接，pin HBM 物理页    │
│ → SG 表 → UMMU matt_map → NIC 直接 DMA 显存           │
└─────────────────────────────────────────────────────────┘
```

## 2. Phase 1 已完成内容（SPDK 层）

| 文件 | 改动 |
|------|------|
| `examples/nvme/urma_perf/urma_perf_npu.h/.c`（新增） | AscendCL 动态加载器（aclInit/aclrtSetDevice/aclrtCreateContext/aclrtMalloc/aclrtFree/aclrtMemcpy）、HBM 分配注册表、NPU memory provider（pin/unpin；export_dmabuf 有意留 NULL） |
| `examples/nvme/urma_perf/urma_perf.c`（修改） | 内存路线枚举新增 `URMA_PERF_MEM_NPU` / `URMA_PERF_MEM_NPU_STAGED`；`-M npu` / `-M npu-staged` 参数；设备无关拷贝包装（dev_memcpy_h2d/d2h、dev_thread_bind）；分配/预检/清理/结果打印分流；NPU memory domain |
| `examples/nvme/urma_perf/Makefile`（修改） | 挂接 urma_perf_npu.c |
| `mk/nvme.libtest.mk`（修改） | 支持 `C_SRCS-y` 附加源文件（其他示例不受影响） |

**零改动**：`include/spdk/nvme_urma.h`、`lib/nvme/*`、`lib/nvmf/*`、同事B 的全部 CUDA 代码路径。

### 2.1 关键设计点

1. **内存类型识别**：SPDK 传输层 `nvme_urma_req_memory_type()`（lib/nvme/nvme_urma.c）
   按 memory domain 的 id 字符串识别设备类型：包含 `npu` 或 `ascend` 即映射为
   `SPDK_NVME_URMA_MEM_NPU`。因此 NPU domain 命名为 `npu:spdk-urma-perf`。
2. **两条 NPU 路线**：
   - `npu`（对应 GPU 的 peermem）：HBM 整块注册，provider pin 后由注册层走
     `urma_register_seg(is_gpu_seg=1)` peer-memory 路线。**依赖 Phase 2 内核
     NPU 桥接模块**，当前内核只有 NVIDIA 桥接，预期注册失败。
   - `npu-staged`（对应 posix）：I/O 缓冲是 host 暂存（4K 对齐），每 I/O 经
     aclrtMemcpy 与 HBM 影子缓冲分级拷贝。不依赖内核改动，可全链路验证。
3. **dmabuf 路线未实现**：AscendCL 无公开的 dmabuf 导出 API（无
   cuMemGetHandleForAddressRange 等价物），provider 的 export_dmabuf 留 NULL。
   注册层遇到 NULL 会自动走 peer-memory 分支。
4. **dlopen 方式**：与 CUDA 侧一致，编译不依赖 CANN 头文件/库；无 CANN 的机器
   上 `-M cpu` 等路线不受影响。`URMA_PERF_NPU_LIB` 环境变量可指定库路径。
5. **HBM 对齐**：`URMA_PERF_NPU_ALIGNMENT` 默认 64KB（镜像 CUDA），上机后按
   实测 NPU 页粒度校准。
6. **线程绑定**：`aclrtSetCurrentContext`（旧版 CANN 缺失时回退
   `aclrtSetDevice`），供 npu-staged 的多 worker 线程做拷贝。

## 3. Phase 2 方案要点（UMDK / 内核驱动，待实现）

### 3.1 现状（GPU 实现的事实）

- 内核 `udma_gpu_p2p.c` 导出 `udma_register_gpu_p2p_ops()` /
  `udma_unregister_gpu_p2p_ops()`，provider 是函数指针表
  `struct udma_gpu_p2p_ops`（get_pages/put_pages/free_page_table/owner）。
  该设计为规避 GPL/proprietary taint（照抄 ib_core.ko ↔ nvidia-peermem.ko 模式）。
- 契约强耦合 NVIDIA：`get_pages` 返回 `struct nvidia_p2p_page_table *`，
  且强制 64KB 页（`NVIDIA_P2P_PAGE_SIZE_64KB` 校验）。
- **全局单槽**：已注册 provider 时再注册返回 -EBUSY，GPU/NPU 无法共存。
- 用户态到内核的标记 `struct udma_register_seg_ucmd { is_gpu_seg; reserved; }`
  走 `udrv_data` 私有通道，`reserved` 目前未用。

### 3.2 NPU 接入需要的三件事

1. **契约泛化**（方案二选一，上机验证后定）：
   - A. NPU 桥接直接产出布局兼容的 page_table 结构（entries/page_size/pages[]）
     ——改动最小，但 NPU 页粒度若非 64KB 需放宽校验；
   - B. 把 `struct udma_gpu_p2p_ops` 泛化为通用页描述（PA 数组 + 页大小），
     NVIDIA 桥接做一层适配——更干净，改动面稍大。
2. **多槽 + 类型选择**：`is_gpu_seg` 从 1 bit 扩展为设备类型（可用
   `udma_register_seg_ucmd.reserved` 字段携带），内核按类型路由到对应 provider。
3. **NPU 桥接模块 `udma_npu_bridge.ko`**：调 davinci 驱动接口 pin NPU HBM
   物理页，产出 SG 表。**可行性取决于昇腾内核驱动是否导出可用的 pin 接口——
   这是 Phase 2 第一验证项。**

### 3.3 UMDK 用户态改动点

- `urma_types.h`：`urma_seg_cfg_t` 的 `is_gpu_seg` 扩展为设备类型字段
  （或新增 `dev_type`，保留 `is_gpu_seg` 兼容）。
- `udma_u_segment.c`：`udma_register_seg_ucmd` 填入设备类型。

## 4. 上公司机器后必须验证的三个问题

1. **NPU 内核 pin 接口**：davinci 驱动是否导出能返回 HBM 物理页的符号
   （类似 nvidia_p2p_get_pages）？决定 udma_npu_bridge.ko 怎么写。
   验证方法：`cat /proc/kallsyms | grep -i davinci` 查导出符号；
   检查 CANN 驱动包内核模块（dvpp / davinci 等 .ko）的 Module.symvers。
2. **CANN dmabuf 导出能力**：是否存在未公开/新版本的 HBM dmabuf 导出接口？
   若有，dmabuf 路线可绕过内核 pin，Phase 1 的 provider 只需补 export_dmabuf。
   验证方法：`nm -D libascendcl.so | grep -i -E "dmabuf|fd|handle"`。
3. **NPU HBM 页粒度**：peer-memory pin 的最小粒度是否 64KB？
   影响 URMA_PERF_NPU_ALIGNMENT 与内核对齐逻辑。
   验证方法：跑 `-M npu` 观察注册失败时的对齐报错信息。

## 5. Phase 1 验证步骤（公司 Linux 机器）

```bash
# 编译（UMDK 路径按实际部署替换）
./configure --with-urma=/path/to/UMDK_tool_netlab
make -j

# 1. 回归：确认未破坏现有路线
./build/examples/urma_perf -r 'trtype:URMA adrfam:IPv4 traddr:<ip> trsvcid:4420 subnqn:nqn.2016-06.io.spdk:cnode1' -M cpu -t 5

# 2. NPU staged 路线（不依赖内核改动，全链路）
./build/examples/urma_perf -r '...' -M npu-staged -t 5

# 3. NPU peermem 路线（预期注册失败，收集报错给 Phase 2）
./build/examples/urma_perf -r '...' -M npu -t 5
```
