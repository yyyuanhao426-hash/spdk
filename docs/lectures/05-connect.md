# 第 5 讲：建连与 Connect 的三种结局

> **本讲只讲一件事**：一条 qpair 的建连分两半——前半是六步同步准备，后半是提交 Connect 命令后等三种结局。
>
> 前置：04（循环与状态机）。

---

## 一图

```
state = 1  CONNECT_ADMINQ
│
├─ ① spdk_urma_device_open        打开 URMA 设备（context / EID）
├─ ② nvme_urma_create_jetty       建 JFR + jetty（收发端点）
├─ ③ nvme_urma_connect_socket     TCP 连到目标 4420
├─ ④ nvme_urma_exchange_hello     交换 EID / jetty id / 深度
├─ ⑤ nvme_urma_capsule_resources_init
│      └─ register_memory → urma_register_seg   注册接收缓冲
└─ ⑥ nvme_fabric_qpair_connect_async
       在 qpair 上提交 Connect 命令，并设 500ms 截止时间
       │
       ▼
   state = 2  WAIT_FOR_CONNECT_ADMINQ
       │  每轮 poll 一次 CQ，看有没有回应
       │
       ├── 结局 ①：收到成功 CR        → 继续自检
       ├── 结局 ②：收到错误 CR        → -5  → 拆 qpair
       ├── 结局 ③：对端断开 / EOF     → -104 → 拆 qpair
       └── 结局 ④：500ms 内什么都没有 → -125 → 拆 qpair
```

---

## 5.1 前半：六步同步准备

调用链：

```
nvme_transport_ctrlr_connect_qpair            lib/nvme/nvme_transport.c
└─ nvme_urma_ctrlr_connect_qpair              lib/nvme/nvme_urma.c
   ├─ spdk_urma_device_open                   lib/nvme/nvme_urma_common.c
   ├─ nvme_urma_create_jetty
   ├─ nvme_urma_connect_socket
   ├─ nvme_urma_exchange_hello
   ├─ nvme_urma_capsule_resources_init
   └─ nvme_fabric_qpair_connect_async         lib/nvme/nvme_fabric.c
```

每一步可能返回的错误：

| 步 | 做什么 | 典型失败码 |
|---|---|---|
| ① | 打开设备、创建 context | `-EINVAL` `-ENODEV` `-EIO` `-ENOTSUP` `-ENOMEM` |
| ② | 建 JFR/jetty | `-EIO` `-ENOMEM` `-EMSGSIZE` |
| ③ | TCP connect | `-errno`：`ECONNREFUSED(111)` `ETIMEDOUT(110)` `EHOSTUNREACH(113)` |
| ④ | hello 收发（阻塞，5s 超时） | `-EPROTO` `-ECONNRESET(104)` `-EAGAIN(11)` |
| ⑤ | 注册接收缓冲 | `-ENOTSUP` `-ENOMEM`、`-errno`（如 `-2`、`-EINVAL`） |
| ⑥ | 提交 Connect 命令 | `-ENXIO` `-EAGAIN` `-EINVAL` |

> **注意 ④ 的坑**：hello 是阻塞收发，socket 设了 5s 超时。
> `recv()` 超时返回 `EAGAIN`，`nvme_urma_read_full` 会把它变成 **`-11`**。
> 这个 `-11` 看起来像轮询层的"还没好"，其实是**等了 5 秒超时**。
> 同一个数字，在不同层含义不同（见 06 讲）。

这一阶段失败的特点：**还没有任何 CR**。日志里表现为：

```
4505 Ctrlr operation failed with error: X, ctrlr state: 57 (error)
```

前面**没有** `fatal rc=`，**没有** `stage=cr_error`。

---

## 5.2 Connect 命令长什么样

```c
/* nvme_fabric_qpair_connect_async */
cmd.opcode = SPDK_NVME_OPC_FABRIC;
cmd.fctype = SPDK_NVMF_FABRIC_COMMAND_CONNECT;
cmd.qid    = qpair->id;              /* 0 = admin，1..N = io */
cmd.sqsize = num_entries - 1;
cmd.kato   = ctrlr->opts.keep_alive_timeout_ms;

/* 附带一段数据：hostnqn、subnqn、hostid、cntlid */
nvmf_data->subnqn = ctrlr->trid.subnqn;
```

提交后，在 URMA 上是"发一个胶囊（capsule）"。对端处理完，会回一个**完成记录 CR**。

每条 CR 里带着：

| 字段 | 含义 | 日志里叫 |
|---|---|---|
| `status` | 完成状态（0=成功） | `status=` |
| `user_ctx` | 这是哪个请求的完成 | `user_ctx=` |
| `flag.bs.s_r` | 0=发送侧，1=接收侧 | `s_r=` |
| `flag.bs.jetty` | 是不是 jetty 的 CR | `flags=` 里的位 |
| `local_id` / `tpn` | 本地 jetty id / 传输号 | `local_id=` `tpn=` |

---

## 5.3 后半：三种结局

### 结局 ①：成功

`connect_poll` 拿到成功完成，设置 `qpair` 为已连接，状态机继续往下（第 06 讲的 READY 之路）。

### 结局 ②：收到错误 CR

```c
/* nvme_urma_process_capsule_cr_inner */
if (cr->status != URMA_CR_SUCCESS) {
        SPDK_ERRLOG("URMA_DIAG role=initiator stage=cr_error ... status=%d ...");
        nvme_urma_qpair_defer_error(owner, -EIO);   /* 一律翻成 -5 */
        return 0;
}
```

日志形状：

```
URMA_DIAG stage=cr_error qid=1 ctx_type=1 status=N flags=0x.. s_r=0 ...
capsule completion error routed to qpair qid=1: err=-5
fatal rc=-5, disconnecting for reconnect
```

**`status=N` 才是真信息**。URMA 状态枚举（UMDK `urma_opcode.h`）：

```
0  SUCCESS
1  UNSUPPORTED_OPCODE
2  LOC_LEN
3  LOC_OPERATION
4  LOC_ACCESS
5  REM_RESP_LEN
6  REM_UNSUPPORTED_REQ
7  REM_OPERATION
8  REM_ACCESS_ABORT
9  ACK_TIMEOUT
10 RNR_RETRY_CNT_EXC     ← 对端接收队列没有 buffer
11 WR_FLUSH
...
```

> 版本不同枚举可能不同。复跑前确认 initiator 用的 SDK 头文件。

### 结局 ③：对端断开

两种触发：

- bootstrap socket 读到 EOF（`nvme_urma_check_lifetime_socket`）；
- CR 的 `s_r=1`（对端标记了关闭）。

日志形状：

```
fatal rc=-104, disconnecting for reconnect
```

### 结局 ④：超时，什么都没有

```c
/* nvme_wait_for_completion_poll, lib/nvme/nvme.c */
if (!status->done && status->timeout_tsc && now > status->timeout_tsc)
        goto error;
error:
        if (!status->done) status->timed_out = true;
        return -ECANCELED;        /* -125 */
```

`nvme_fabric_qpair_connect_poll` 看到 `timed_out`：

```c
NVME_QPAIR_ERRLOG(qpair, "Connect command failed, rc %d ...",
                  status->timed_out ? -ECANCELED : -EIO);   /* 打印 -125 */
```

日志形状：

```
fabric.c:617 Connect command failed, rc -125
fatal rc=-125, disconnecting for reconnect
URMA_DIAG stage=fatal ... rc=-125 fd=104
```

**没有 `stage=cr_error` 是正常的**——因为根本没有收到任何 CR。
"日志里没有"本身就是证据。

---

## 5.4 500ms 从哪来

`lib/nvme/nvme_internal.h`：

```c
#ifdef DEBUG
#define NVME_FABRIC_CONNECT_COMMAND_TIMEOUT 0        /* 调试构建：永不超时 */
#else
#define NVME_FABRIC_CONNECT_COMMAND_TIMEOUT 500000   /* 生产构建：500ms */
#endif
```

`nvme_fabric_qpair_connect_async` 里设置：

```c
if (ctrlr->opts.fabrics_connect_timeout_us > 0) {
        status->timeout_tsc = now + fabrics_connect_timeout_us * ticks_per_us;
}
```

应用可以覆盖（mooncake wrapper）：

```bash
MC_NVME_FABRICS_CONNECT_TIMEOUT_US=5000000 ./your_app
```

**推论**：能看到 `-125`，说明超时被启用（>0），即非 DEBUG 构建或显式设置过。

---

## 5.5 为什么错误要"记到 owner 身上"

一个 poller 线程收到的 CR 可能属于另一个 qpair：

```c
nvme_urma_qpair_defer_error(owner, -EIO);
/* 写进 owner->deferred_error，owner 下一次 poll 时自己取出来 */
```

所以日志里可能出现 `owner=` 和 `poller=` **不是同一个地址**。
这不是 bug，是设计：**错误必须归到出问题的那条连接上**。

---

## 5.6 admin qpair 与 io qpair 的区别

两者建连流程**完全一样**，区别只有两点：

| 项 | admin（qid=0） | io（qid≥1） |
|---|---|---|
| 失败后 | `nvme_ctrlr_fail()`：整个控制器判死 | 只拆这条 qpair，**ctrlr 仍可用** |
| 失败后的日志 | `Ctrlr is in error state` | `alloc_io_qpair failed` |

判断办法很简单：日志里有没有 `alloc_io_qpair`。

---

## 常见误解

| 误解 | 实际 |
|---|---|
| "没有 status 就是日志缺了" | 超时/断开时本来就没有 CR |
| "`-5` 是真正的错误原因" | 是垃圾桶翻译，真信息在 `stage=cr_error status=` |
| "`sc 8` 是对端拒绝" | 是**本地中止**（ABORTED_SQ_DELETION） |
| "`-11` 永远是'还没好'" | 在阻塞收发层可能是 5 秒超时 |
| "管理员队列和数据队列建连不一样" | 流程一样，只是失败后的处理不同 |
| "超时说明对端死了" | 只说明**500ms 内没回应**，对端可能只是慢 |

---

## 练习

1. 日志里只有 `fabric.c:617 rc -125`，没有 628。属于哪种结局？为什么没有 628？
2. `fatal rc=-5` 和 `fatal rc=-104`，分别去看哪一行才能知道根因？
3. 一条 io qpair（qid=1）失败后，为什么没有 `Ctrlr is in error state`？

---

## 下一讲

**06-rc.md**：把 `-2 / -5 / -6 / -11 / -19 / -104 / -125 / -1` 一次讲清楚。
