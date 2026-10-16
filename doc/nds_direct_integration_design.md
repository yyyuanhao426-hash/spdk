# NDS 直连路线 SPDK 集成设计 v2（RA/HCCP 注册 + 目标侧导入数据面）

> 状态：实施设计（可执行） ｜ 前置：[nds_phase2_summary.md](nds_phase2_summary.md)、
> [nds_direct_integration_design.md](nds_direct_integration_design.md)（v1 方案对比，已被本稿取代）
> 依据：P2-19 判决实验通过——RA/HCCP 注册的 HBM 段可被 URMA jetty（CTP）远端 READ/WRITE

## 1. 设计输入（已确认事实，全部有实测依据）

| # | 事实 | 出处 |
|---|------|------|
| F1 | HBM device VA 可经 RA/HCCP `RaCtxLmemRegister(nonPin)` 注册，产出 {MemKey, targetSegHandle, eid, uasid, va, token} | P2-19 探针（nds_buf_register=0） |
| F2 | RA 注册的 HBM 段可被远端 `urma_import_seg(ubva{eid,uasid,va})` + `urma_import_jetty(CTP)` + URMA READ/WRITE 访问，数据逐字节一致 | P2-19 双进程判决 |
| F3 | 裸 `urma_register_seg(non_pin=1)` 注册的段**不可**被远端 jetty DMA（LOC_ACCESS_ERR/无 CQE）——注册通道的选择是决定性的 | P2-8 判决 |
| F4 | SPDK 现注册流：非 host 类型 → provider->pin() → urma_register_seg(is_gpu_seg=1)（内核 peer-memory，133 缺框架必败）；export_dmabuf 钩子存在但 CANN 无公开 dmabuf 导出 | nvme_urma_common.c:307-386 |
| F5 | memfabric 的自动 provision 会写系统 OPP 树+ini（SOP：跑前/跑后检查回滚） | P2-14 |
| F6 | 编译必须与 CCDK 对齐（-I/usr/include/ub/umdk/urma + liburma.so；umdk/src 头布局不一致会 EPERM） | P2-19 工程要点 |

## 2. 目标架构（端到端数据通路）

```
urma_perf（initiator，NPU 侧）                nvmf_tgt（target，host 侧）
┌────────────────────────────┐          ┌──────────────────────────────┐
│ aclrtMalloc HBM            │          │                              │
│   ↓ 写 pattern             │          │  urma_import_seg(ubva{eid,   │
│ nds_buf_register(HBM)      │          │    uasid,va}, NOMAP)         │
│   = RaCtxLmemRegister      │   TCP    │    ↓ 导入成功 = 对端 HBM     │
│     (nonPin)  [libra.so]   │──HELLO──►│      在本进程可寻址           │
│ URMA jetty(CTP)            │ 段信息交换│  URMA jetty(CTP)             │
│   （数据原地，零拷贝）       │          │   ↓ URMA READ/WRITE          │
│                            │          │   iobuf → AIO → NVMe 落盘    │
└────────────────────────────┘          └──────────────────────────────┘
```

**关键语义**：initiator 的 HBM 经 RA 注册后，target 通过 urma_import_seg
将其映射进**自己的设备地址空间**——此后 target 的 jetty 直接对这段 HBM
做远端 READ/WRITE（NVMe 数据面），**initiator 进程零参与数据搬运**
（对比 npu-staged：initiator 每笔 I/O 做 D2H 拷贝）。

## 3. 模块设计

### 3.1 模块 A：NPU provider 改造（examples/nvme/urma_perf/urma_perf_npu.c）

| 改动点 | 内容 |
|--------|------|
| A1 | npu_driver 增加 libra.so 加载（RaInit/RaCtxInit/RaCtxQpCreate，照 CCDK plugin_loader.c 的 dlopen 序列） |
| A2 | 新增 `npu_nds_register(hbm_va, len)`：封装 RaCtxLmemRegister(nonPin=ENABLE)，flags 照 P2-19（tokenPolicy=NONE/tokenIdValid=DISABLE/RW\|ATOMIC/cacheable=DISABLE），产出段信息存入 npu_alloc_entry |
| A3 | npu_alloc_entry 增加字段：`struct nds_seg_info {u64 eid[16]; u32 uasid; u64 va; u32 token;}`（P2-19 实测格式） |
| A4 | provider 接口扩展：新 op `get_segment_info(pin_handle, &seg_info)`——供传输层把段信息交换给对端 |

**不做**：删除 is_gpu_seg 路径（npu-staged 迁移期保留）；不改 aclrtMalloc
分配逻辑。

### 3.2 模块 B：段信息交换（lib/nvme/nvme_urma.c）

HELLO 交换（nvme_urma_exchange_hello）现有 `spdk_urma_endpoint_desc`
{eid, jetty_id, transport_mode, max_queue_depth, max_io_size}——eid/jetty
已有，**缺 uasid 与段信息**。

| 改动点 | 内容 |
|--------|------|
| B1 | endpoint_desc 追加 `u32 uasid`（或新消息类型 SPDK_URMA_MSG_SEG_INFO，避免破坏 packed 布局兼容性——**推荐新消息**，旧对端忽略未知类型即可） |
| B2 | 新消息 SPDK_URMA_MSG_SEG_INFO：承载 {eid, uasid, va, token, len}（initiator 的 HBM 段），target 收到后执行 urma_import_seg |

### 3.3 模块 C：target 侧数据面（lib/nvme/nvme_urma.c）

| 改动点 | 内容 |
|--------|------|
| C1 | nvmf_tgt 侧收到 SEG_INFO 后：urma_import_seg(ubva{eid,uasid,va}, NON_CACHEABLE, RW\|ATOMIC, SEG_NOMAP) → 本地段句柄 |
| C2 | I/O 数据面：target 的 jetty 对导入段做 URMA READ（NVMe 写命令：从对端 HBM 拉数据）/ URMA WRITE（NVMe 读命令：把盘数据推进对端 HBM）——**target 主动拉取/推送，initiator 零参与** |
| C3 | 传输模式分流：NPU 类型内存走 C1/C2 导入路径；host 类型内存保持既有 URMA 传输（npu-staged/cpu 不受影响） |

### 3.4 模块 D：编译与链接对齐

- urma_perf/nvmf_tgt 链接：-I/usr/include/ub/umdk/urma + liburma.so
  （**禁用 umdk/src 的 urma_api.h**——布局不一致 EPERM，P2-19 教训）
- 新增链接：libra.so（dlopen 则免）
- 依赖清单：libra.so / liburma.so / libtsdclient.so / libacl_rt.so
  （133 全在位，P2-16/C 项）

## 4. 数据通路语义（与 npu-staged 的对照）

| 维度 | npu-staged（中转） | 本设计（直连） |
|------|--------------------|----------------|
| 发起方 | initiator 每笔 I/O 做 D2H 拷贝 | **target 主动拉取/推送，initiator 零参与** |
| 拷贝次数 | 2（HBM→host、host→target iobuf） | **1**（URMA READ/WRITE 直达 HBM 段） |
| initiator CPU | 每笔 I/O 参与 | **零参与**（推理主流程不被存储 I/O 打断） |
| HBM 注册 | host 暂存注册（host 内存） | RA/HCCP 注册 HBM 本体 |
| 依赖 | 无内核改动 | nvme_nds 驱动（133 已内置） |

## 5. 验证计划

| 阶段 | 内容 | 判定 |
|------|------|------|
| V1 | 探针级：RA 注册 + import + 单块读写（=P2-19 已通过） | ✅ 复现 |
| V2 | SPDK 级：urma_perf -M npu（新数据面）单 I/O 双向数据正确 | NVMe WRITE 后 target 拉取的数据 == HBM pattern |
| V3 | 全链路：直连版 -M npu 完整跑通（对 npu-staged 同条件对比） | IOPS/延迟/CPU 占用对比报告 |

## 6. 风险与开放问题

| # | 风险 | 缓解 |
|---|------|------|
| 1 | urma_import_seg 的 ubva 寻址在 SPDK 多队列/多连接场景的语义（每 qpair 一套 uasid？） | V2 首测单 qpair；多 qpair 问题实测暴露后再设计 |
| 2 | imported 段的生命周期（qpair 断开时 unimport/unreg 顺序） | C3 明确释放顺序，异常路径逐一核对 |
| 3 | NPU 进程异常退出导致段泄漏 | urma_admin show 巡检 + 重启恢复路径已验证（P2-14） |
| 4 | 133 生产负载（vLLM）共享 NPU 的资源竞争 | 测试窗口协调，选卡避开在用卡 |

## 7. 实施批次

| 批次 | 内容 | 执行侧 |
|------|------|--------|
| P2-20 | 模块 A：NPU provider RA/HCCP 注册 + get_segment_info（外部开发，nds_v1 push） | 外部 |
| P2-21 | 模块 B/C：段信息交换 + target 导入 + 数据面（外部开发） | 外部 |
| P2-22 | 133 上构建 + V2 验证（urma_perf -M npu 双向数据判决） | 测试 Agent |
| P2-23 | V3 全链路直连首测 + 性能对比（直连 vs 中转） | 测试 Agent |
