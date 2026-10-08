# NDS 直连路线集成设计（基于 RA/HCCP 注册 + nvme_nds，v1）

> 状态：设计稿 v1（待评审） ｜ 前置阅读：[nds_phase2_summary.md](nds_phase2_summary.md)
> 依据：P2-17（盘→HBM 读方向实测一致）、P2-18（HBM→盘 写方向实测一致）、
> P2-19（RA/HCCP 注册段被 URMA jetty 远端读写判决通过）

## 1. 已确认的机制基础

133 上实测打通的直连数据面（双向逐字节一致）：

```
NPU HBM（aclrtMalloc，HUGE_FIRST）
  ⇅ RaCtxLmemRegister(nonPin=ENABLE)          ← libra.so（RA/HCCP，dlopen）
  ⇅ 产出 MemKey / targetSegHandle / 段信息 {eid, uasid, va, token}
内核 nvme_nds 驱动（NVMe-over-UB）
  ⇅ NDS_IOCTL_FILE_TRANSFER / 块设备路径       ← /dev/nvme-nds
UB-SSU 裸盘（/dev/nvme1n1，4G，已挂载 ext4）
```

关键事实：
- 该路径**无需** hcomm/CASM 共享、**无需** AICPU kernel、**无需** OPP 安装、
  **无需** 签名包——绕开了 P2-0~P2-13 的全部部署门槛
- RA/HCCP 注册的 HBM 段**可被 URMA jetty（CTP）远端读写**（P2-19 判决），
  而裸 non_pin 注册的段不可（P2-8 判决）——**注册通道的选择是决定性的**
- 块设备路径（nds_block_io_common）数据正确；文件路径
  （nds_file_io_common）存在 FIEMAP 翻译缺陷（同事C 代码，返回全 0）

## 2. SPDK 现状：注册流的真实代码路径

`lib/nvme/nvme_urma_common.c: spdk_nvme_urma_register_memory()`：

```
① 非 host 类型 → 调 provider->pin()            （urma_perf_npu.c 的
                                                  npu_provider_pin，仅登记）
② 填 urma_seg_cfg_t：is_gpu_seg=1（非 host），
   token=DEFAULT、token_policy=NONE、RW|ATOMIC
③ 若 provider->export_dmabuf != NULL → 走 dma-buf 注册分支
   （spdk_urma_register_seg_dmabuf）—— NPU provider 当前为 NULL
④ target_seg==NULL → urma_register_seg(&cfg)   ← 内核 peer-memory 路径
   （is_gpu_seg=1），依赖 udma.ko 的 gpu_p2p 框架
   —— **133 内核未启用（ENABLE=0），此路不通（P2-2/P2-8/P2-15）**
```

即：当前 NPU provider 的 pin() 之后，注册必然落入 ④ 的内核 peer-memory
路径——在 133 上死路。同时②的 is_gpu_seg=1 走 udrv_data 侧信道，内核
同样无响应框架。

## 3. 集成方案对比

### 方案 A（推荐）：nds 数据面直驱——SPDK 侧零内核依赖

urma_perf 的 `-M npu` 数据面改用 **libnds**（同事C 的 CCDK）：
- HBM 注册：`nds_init` + `nds_buf_register`（内部 RaCtxLmemRegister
  nonPin——P2-17 实测成功）
- 数据面：`nds_file_io` / `nds_batch_io`（ioctl NDS_IOCTL_FILE_TRANSFER，
  内核 nvme_nds 完成 HBM↔UB-SSU 搬运——P2-17 读/P2-18 写实测一致）
- SPDK 角色：urma_perf 作为 nds 数据面的驱动器与校验框架；SPDK 的
  NVMe-over-URMA 传输层**保持 host 内存路线不变**（cpu/npu-staged 路线
  不受影响）

改动面：仅 urma_perf_npu.c/urma_perf.c（+链接 libnds）；lib/nvme 传输层
零改动；内核零改动。
风险：nds 栈依赖 nvme_nds 驱动的 UB-SSU（133 已具备）；文件路径 FIEMAP
缺陷需绕过（块路径已验证）。

### 方案 B：SPDK 深度集成——扩展 nvme_urma 注册流支持 RA 段

为 NPU 类型在 nvme_urma_common.c 增加"RA 注册 + 段信息导出"分支：
- provider 增加 export_segment op（返回 {eid, uasid, va, jetty_id,
  token_id}）
- nvmf_tgt 侧 urma_import_seg(ubva) + urma_import_jetty(CTP) 导入后，
  用 URMA jetty 远端读写 HBM 段（P2-19 判决该组合可行）
- wire 协议需扩展 HELLO/元数据交换以承载段信息

改动面：lib/nvme/nvme_urma.c/common.c + wire 协议 + 双端——工程量大，
且远端读写语义仍需实测（P2-19 只验证了探针级）。
定位：方案 A 验证通过后的产品化演进方向。

### 方案 C：真零拷贝（URMA 设备 DMA 直达 HBM）

需要 udma.ko 启用 gpu_p2p 框架（源码缺失）或 ummu phys_sg API（内核
缺失）——**当前平台不可达**，依赖华为驱动源码支持。暂列展望。

## 4. 方案 A 实现细节

### 4.1 urma_perf_npu.c 改造点

1. **新增 nds 数据面模块**（nds_data_plane.c 或并入 npu provider）：
   - `nds_init()`：dlopen libra.so（RaInit/RaCtxInit/RaCtxQpCreate，
     照 CCDK plugin_loader.c）+ 打开 /dev/nvme-nds
   - `nds_buf_register(hbm_va, len)`：RaCtxLmemRegister(nonPin=ENABLE)
     （flags 照 P2-19：tokenPolicy=NONE、tokenIdValid=DISABLE、
     RW|ATOMIC、cacheable=DISABLE）
   - `nds_transfer_read/write(...)`：nds_file_io_common（块路径，
     绕过文件路径 FIEMAP 缺陷）或 nds_batch_io
2. **urma_perf.c 的 -M npu 数据路径分流**：I/O 缓冲 = RA 注册的 HBM；
     数据面调用 nds_transfer 而非 SPDK NVMe I/O（直连模式）；对比模式
     （-M npu-staged）保持 SPDK NVMe I/O
3. **编译**：链接 libnds.so（CCDK 产物）或直接编入 nds 源码；头文件
   /usr/include/ub/umdk/urma（与 CCDK 对齐，禁用 umdk/src 头——结构体
   布局不一致会 EPERM，P2-19 教训）

### 4.2 两阶段验证设计

| 阶段 | 内容 | 判定 |
|------|------|------|
| V1 功能 | nds_transfer 单块读/写（HBM↔UB-SSU）双向一致 | 复现 P2-18 |
| V2 性能 | nds-bench 各 I/O 尺寸/队列深度；与 AIO 基线对比 | 产出直连路线性能基线 |

### 4.3 已知问题

1. 文件路径 FIEMAP 翻译缺陷（同事C nds.c）——块路径绕过，缺陷反馈
   上游
2. nvme_nds refcount 高（他租户在用）——测试需协调或观察式进行
3. NPU4 Critical、NPU0 HBM 近满——选卡动态确认

## 5. 实施计划

| 批次 | 内容 | 前置 |
|------|------|------|
| P2-20 | libnds 集成进 urma_perf（nds_init/register/transfer 三接口）+ 编译 | 无 |
| P2-21 | V1 功能验证（单块双向） | P2-20 |
| P2-22 | V2 性能基线（各尺寸/队列深度）+ 与 AIO 对照 | P2-21 |
| P2-23 | SPDK 集成评审（基于 V2 数据决定方案 B 是否启动） | P2-22 |

## 6. 开放问题

1. libnds 的打包/安装形态（CCDK build.sh 产出的 libnds.so 是否可独立
   dlopen，还是需要完整 CANN env）
2. nds_transfer 的并发模型（多队列/多线程是否安全）
3. RA 注册段的生命周期管理（进程退出/异常时的清理路径）
