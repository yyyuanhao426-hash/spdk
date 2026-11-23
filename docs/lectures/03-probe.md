# 第 3 讲：probe——只报名，不联网

> **本讲只讲一件事**：`spdk_nvme_probe` 在 fabric 传输上退化成"填表登记"，它返回 0 只表示流程结束，不表示连上。
>
> 前置：01（对象）、02（回调）。

---

## 一图

```
你的线程                                              SPDK
   │                                                   │
   │  spdk_nvme_probe(trid, info, probe_cb, attach_cb) │
   ├──────────────────────────────────────────────────▶│
   │                                                   │ ① calloc probe_ctx（差旅袋）
   │                                                   │ ② probe_cb → "要这块"
   │                                                   │ ③ construct ctrlr（state=1）
   │                                                   │ ④ 挂进 init_ctrlrs
   │                                                   │
   │                                                   │ ⑤ while(poll) 一圈圈推进
   │                                                   │      ├─ attach_cb      （成功）
   │                                                   │      └─ attach_fail_cb （失败）
   │                                                   │
   │◀────────────────── return 0 ──────────────────────┤ ⑥ 清单空了才返回
   │
   ▼
   这时 attach_cb 已经（或已经不会）被调用过了
```

**关键时序**：同步版 `spdk_nvme_probe` 会**阻塞到所有待办都结束**；`attach_cb` / `attach_fail_cb` 是在它**返回之前**、在它内部被调用的。

---

## 3.1 "probe" 这个词

原意：**探测、发现总线上有哪些设备**。

| 传输 | probe 实际做什么 |
|---|---|
| PCIe | 扫 PCI 总线，发现几块盘就有几个设备 |
| **URMA / RDMA / TCP** | 没有总线可扫。你给的 `trid` **就是那个设备的地址**，所以"扫描"退化成"造一个 ctrlr 对象" |

代码里的退化路径（`lib/nvme/nvme_fabric.c`，`nvme_fabric_ctrlr_scan`）：

```c
if (strcmp(probe_ctx->trid.subnqn, SPDK_NVMF_DISCOVERY_NQN) != 0) {
        /* 不是 discovery：直接连这个地址 */
        rc = nvme_ctrlr_probe(&probe_ctx->trid, probe_ctx, NULL);
        return rc;
}
```

只有当你连的是 **discovery NQN**（NVMe-oF 的"目录服务"）时，probe 才恢复本意：先连一个临时控制器拿子系统列表，再逐个连。你的日志里 `subnqn=nqn.2026-01.io.spdk:urma-gpu-test` 不是 discovery，走的是直接路径。

---

## 3.2 报名阶段干了什么（不发一个网络包）

```
nvme_probe_internal                         lib/nvme/nvme.c
 └─ nvme_transport_ctrlr_scan               lib/nvme/nvme_transport.c
    └─ nvme_fabric_ctrlr_scan               lib/nvme/nvme_fabric.c
       └─ nvme_ctrlr_probe                  lib/nvme/nvme.c
          ├─ probe_cb(...)                  ← 问你要不要
          ├─ nvme_transport_ctrlr_construct ← 只分配对象
          └─ TAILQ_INSERT_TAIL(&probe_ctx->init_ctrlrs, ctrlr)
```

**这一段里，一个 URMA 包都没发。** 真正的建连在后面的循环里（第 04 讲）。

---

## 3.3 probe_ctx：SPDK 的"差旅袋"

```c
struct spdk_nvme_probe_ctx {
        struct spdk_nvme_transport_id  trid;           /* 要连谁 */
        const struct spdk_nvme_ctrlr_opts *opts;       /* 参数 */
        void                          *cb_ctx;         /* 你的袋子 */
        spdk_nvme_probe_cb             probe_cb;       /* 你留的电话 */
        spdk_nvme_attach_cb            attach_cb;
        spdk_nvme_attach_fail_cb       attach_fail_cb; /* 没留就用默认 */
        spdk_nvme_remove_cb            remove_cb;
        TAILQ_HEAD(, spdk_nvme_ctrlr)  init_ctrlrs;    /* 待办清单 */
        struct spdk_nvme_detach_ctx    failed_ctxs;    /* 失败后慢慢拆的 */
};
```

- 谁创建：`spdk_nvme_probe_async_ext()` 里 `calloc`。
- 活多久：从 probe 开始，到两个清单都清空。
- 为什么需要：**跨轮次、跨函数的状态必须有地方存**。

---

## 3.4 最容易被误解的返回 0

```c
/* lib/nvme/nvme.c, nvme_init_controllers */
while (true) {
        rc = spdk_nvme_probe_poll_async(probe_ctx);
        if (rc != -EAGAIN) return rc;
}
```

`spdk_nvme_probe_poll_async` 只返回两种值：

- `-EAGAIN`：还有待办；
- `0`：两个清单都空了——**不管设备是成功挂上，还是失败拆掉**。

所以：

```
spdk_nvme_probe() == 0
   ├─ 可能：1 个设备挂上了
   ├─ 可能：1 个设备失败了（失败只通过 attach_fail_cb 告诉你）
   └─ 可能：0 个设备
```

应用层要判断"到底有没有连上"，只能看 `attach_cb` 有没有把 ctrlr 交过来。mooncake 的 wrapper 就是这么补的：

```cpp
rc = spdk_nvme_probe(trid, info, probe_cb, attach_cb, NULL);
if (rc != 0) return rc;
return info->ctrlr != nullptr ? 0 : -ENODEV;   /* probe 说 0，但没交钥匙 */
```

> 这就是日志末尾 `rc=-19` 的来源：**wrapper 自己补的 -ENODEV**，不是 SPDK 说"设备不存在"。

---

## 3.5 四个回调在报名阶段的分工

| 回调 | 触发点（报名阶段） | 触发点（后续阶段） |
|---|---|---|
| `probe_cb` | 发现候选设备时，一次 | — |
| `attach_cb` | — | 初始化成功，READY 时 |
| `attach_fail_cb` | 构造失败时 | 初始化失败时 |
| `remove_cb` | — | 热拔时 |

---

## 常见误解

| 误解 | 实际 |
|---|---|
| "probe 成功 = 连上了" | 只表示流程结束；连没连上要看 attach_cb |
| "probe 阶段网络不通会报错" | 这个阶段不发网络包，报错只会来自构造/扫描 |
| "attach_fail_cb 是 attach 之后又失败" | attach = probe→READY 的全过程；attach_fail 是这条路的失败出口 |
| "rc=-19 是内核说没设备" | 是 wrapper 自己补的 -ENODEV |
| "probe 会一个个同步连完" | 它是异步轮询，一轮只推进一步（见 04 讲） |

---

## 练习

1. `spdk_nvme_probe` 返回 0，但 `attach_cb` 没被调用。可能发生了什么？
2. 为什么构造 ctrlr 时只分配、不建连？提示：一个 probe 可能发现多个设备。
3. 如果 `probe_cb` 返回 `false`，后面还会发生什么？在哪里返回？

---

## 下一讲

**04-loop.md**：probe 返回之前，那个把状态机一圈圈推着走的循环。
