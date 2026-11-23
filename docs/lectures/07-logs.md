# 第 7 讲：读日志实战——五问法 + 四个真实案例

> **本讲只讲一件事**：拿到一屏日志，按固定顺序问五个问题，30 秒定位到"哪一幕、哪一层"。
>
> 前置：01–06。
>
> 说明：案例里的 `文件:行号` 来自当时运行的二进制，**行号会随提交漂移**；
> 定位请以**函数名**为准。

---

## 7.1 五问法（按顺序，不要跳）

| # | 问题 | 怎么答 |
|---|---|---|
| 1 | 这一行是谁打的？ | 文件 → 函数 → 哪一幕 |
| 2 | 这是根因还是翻译？ | 找 `cr_error` / `fatal` / `4505` 这类第一手行 |
| 3 | rc 属于哪一类？ | -1 异步收尾；-125 超时；-6 已拆；其它同步失败 |
| 4 | 缺了什么？ | 没有 `cr_error` = 没有 CR；没有 `alloc` = 还在初始化；没有 `628` = 超时 |
| 5 | 哪些是善后？ | `in failed state` / `prop_get` / `CC register` 一律划掉 |

**第 4 问最容易被忽略，但最好用**：日志的"缺失"本身就是证据。

---

## 7.2 日志签名地图（看到这行 → 在哪一幕）

| 签名 | 所在函数 | 含义 |
|---|---|---|
| `attach_cb` 之后的业务开销 | — | 已 READY |
| `spdk_nvme_ctrlr_alloc_io_qpair` | `nvme_ctrlr.c` | 已 READY，在开数据/心跳通道 |
| `nvme_transport_ctrlr_connect_io_qpair() failed` | `nvme_ctrlr.c` | 上面的通道建连失败 |
| `nvme_ctrlr_process_init ... state: N` | `nvme_ctrlr.c` | ctrlr 初始化中（未 READY） |
| `Ctrlr is in error state` | `nvme_ctrlr.c` | 状态机终点，下一轮返回 -1 |
| `URMA_DIAG stage=cr_error` | `nvme_urma.c` | 收到错误 CR（根因行） |
| `URMA_DIAG stage=fatal` | `nvme_urma.c` | 传输层决定拆 qpair（转述） |
| `fabric.c:617 Connect command failed` | `nvme_fabric.c` | Connect 的结局播报 |
| `fabric.c:628 ... sct 0, sc 8` | `nvme_fabric.c` | 有完成事件且被本地中止 |
| `CQ transport error -6` | `nvme_qpair.c` | 已拆（信号） |
| `in failed state` / `prop_get` / `CC register` | `nvme_ctrlr.c` 等 | 拆机善后 |

---

## 7.3 决策树

```
日志里有 alloc_io_qpair 吗？
│
├─ 有 ──▶ 已 READY（使用阶段）
│          │
│          ├─ 有 URMA_DIAG stage=cr_error？
│          │     └─ 有 → 形状①：读 status=
│          ├─ 有 fatal rc=-104？
│          │     └─ 有 → 形状②：对端断开
│          └─ 有 fabric:617 rc=-125？
│                └─ 有 → 形状③：超时无回应
│
└─ 没有 ──▶ ctrlr 初始化中
           │
           ├─ 4505 的 rc = -1  → 异步收尾，看上一轮
           ├─ 4505 的 state = 57 且 rc≠-1 → 同步失败
           │       ├─ 前面有 4447？ → TRANSPORT_READY（URMA 上不该出现）
           │       └─ 没有 4447 → CONNECT_ADMINQ 或提交类 helper
           └─ 4505 的 state = 4/6/8/12/17… → 提交 Property 命令失败
```

---

## 7.4 案例一：对端在握手期间消失

**日志（节选）**

```
nvme_ctrlr.c:1135  in failed state.
nvme_fabric.c:617  Connect command failed, rc -5 ...
nvme_fabric.c:628  Connect command completed with error: sct 0, sc 8
nvme_urma.c:1684   URMA qpair 0: fatal rc=-5, disconnecting for reconnect
nvme_qpair.c:914   CQ transport error -6 (No such device or address)
nvme_ctrlr.c:4463  Ctrlr is in error state
nvme.c:647         Failed to initialize SSD
nvme.c:834         Failed to attach nvme ctrlr: ..., Operation not permitted
wrapper            rc=-19 ctrlr=0
```

**五问**

1. 谁打的：状态机 + fabric + qpair + wrapper，跨 4 层。
2. 根因：`fatal rc=-104`（同一批里另一条）和 `sc 8`；`-5` 是垃圾桶。
3. rc：-1（`Ctrlr is in error state` 后）、-5（Connect 完成错误）、-6（信号）、-19（wrapper）——**没有一个是根因**。
4. 缺了什么：多份日志里能看到 `fatal rc=-104`，说明**对端先断了**。
5. 善后：`Operation not permitted`（rc=-1 的 EPERM 翻译）、`rc=-19`（wrapper 补的 ENODEV）。

**结论**：admin 建连阶段对端消失（形状②）。`sc 8` 是本地中止的痕迹，不是对端拒绝。

---

## 7.5 案例二：io qpair 的 CR 报错（status=10）

**日志（节选）**

```
nvme_urma.c:1380  URMA_DIAG ... stage=cr_error owner=... poller=... qid=1
                  ctx_type=1 status=10 flags=0x82 s_r=0 ... local_id=17356
nvme_urma.c:1344  capsule completion error routed to qpair qid=1: err=-5
nvme_urma.c:1718  URMA qpair 1: fatal rc=-5, disconnecting for reconnect
nvme_qpair.c:914  CQ transport error -6
nvme_ctrlr.c:514  nvme_transport_ctrlr_connect_io_qpair() failed
wrapper           alloc probe spdk_nvme_qpair failed
```

**五问**

1. 谁打的：URMA 传输层（根因）→ qpair → ctrlr → wrapper。
2. 根因：第一行 `status=10`。
3. rc：-5 是翻译；-6 是信号；都不是原因。
4. 缺了什么：没有 `Ctrlr is in error state` → 说明不是 admin 队列（io 失败不判死 ctrlr）。
5. 善后：无。

**解码**

- `ctx_type=1` = `NVME_URMA_CQE_CAPSULE_TX`（发送侧）
- `status=10` = `RNR_RETRY_CNT_EXC`：对端接收队列没有可用 buffer
- `s_r=0`：对端没有标记关闭连接
- `cntlid:1` + 走到 alloc：**admin 已经连上、ctrlr 已 READY**

**结论**：第一条 io qpair 的 Connect 发送时，对端接收侧饿死。本地参数/注册可排除（那些失败不走 CR）。

**下一步**：看对端同一秒的日志（`recv_post` / jetty / quarantine），并确认两侧 `jetty_count`、JFR 深度一致。

---

## 7.6 案例三：admin 建连的同步失败（-2）

**日志（节选）**

```
nvme_ctrlr.c:4505  Ctrlr operation failed with error: -2, ctrlr state: 57 (error)
nvme.c:647         Failed to initialize SSD: ...
nvme.c:834         Failed to attach nvme ctrlr: ..., No such file or directory
nvme_ctrlr.c:1135  in failed state.
nvme_fabric.c:212  Failed to send Property Get fabrics command
nvme_ctrlr.c:1242  Failed to read the CC register
wrapper            rc=-19 ctrlr=0
```

**五问**

1. 谁打的：状态机 → 工头 → 默认 attach_fail → 拆机 → wrapper。
2. 根因：`4505` 的 `-2`，但更准确地说，是 `CONNECT_ADMINQ` 六步里某个函数返回的裸 errno。
3. rc：`-2` 同步失败；后两行是拆机。
4. 缺了什么：没有 `4447`（排除 TRANSPORT_READY）；没有 `alloc`（还在初始化）；没有 `cr_error`（还没到收 CR 的阶段）。
5. 善后：`prop_get` / `CC register` 划掉。

**推理链**

1. `state=57` + `rc≠-1` → 同步失败。
2. 只有 `CONNECT_ADMINQ` 和 `TRANSPORT_READY` 会"置 ERROR 并返回 rc"；
   URMA 的 ops 表**没有** `ctrlr_ready`，所以 55 恒成功 → 排除。
3. 提交类 helper 不会返回 `-2`（它们只会 -ENXIO/-EIO/-EAGAIN/-ENOMEM/-ESHUTDOWN）。
4. `-2` 只能来自建连链里的**裸 errno 泄漏**：
   - `nvme_urma_connect_socket` 的 `-errno`
   - `spdk_nvme_urma_register_memory` 里 `urma_register_seg` 失败后的 `rc = -errno`
5. `No such file` 只是 `spdk_strerror(2)` 对同一个 -2 的第二次翻译。

**结论**：admin 建连在"TCP 连接"或"段注册"阶段失败，最可能是段注册。

**下一步**：看 `4505` 之前的同线程日志（`urma device open` / `URMA IO qpair ... jetty failed` / `set bonding mode failed`）；开 `SPDK_URMA_TRACE=1` 复跑；bare device 与 bonding 对照。

> 背景：bonding 对"注册存储"有额外要求。commit `8f9ce017a` 修过一处——
> bonding 上小管理命令不再用 inline SEND，而是走注册过的 TX 槽。
> 遇到 bonding 独有、bare device 正常的失败，先核对二进制是否包含这类修复。

---

## 7.7 案例四：io qpair 超时（-125）

**日志（节选）**

```
nvme_fabric.c:617  Connect command failed, rc -125 ...
nvme_urma.c:1718   URMA qpair 1: fatal rc=-125, disconnecting for reconnect
nvme_urma.c:1720   URMA_DIAG ... stage=fatal ... rc=-125 fd=104
nvme_qpair.c:914   CQ transport error -6
nvme_ctrlr.c:514   nvme_transport_ctrlr_connect_io_qpair() failed
```

**五问**

1. 谁打的：fabric 播报 → URMA fail → qpair → ctrlr。
2. 根因：**没有 CR**，根因是"500ms 内无任何完成事件"。
3. rc：`-125` 是 SPDK 自己放弃；`-6` 是信号。
4. 缺了什么：没有 `cr_error`（没收到 CR）；没有 `-104`（TCP 还活着）；没有 `628`（不是完成错误）。
5. 善后：无。

**结论**：TCP/hello 已通（对端活着），但 URMA 的 Connect 在 500ms 内没有回应。

**下一步（一次实验定性）**：

```bash
MC_NVME_FABRICS_CONNECT_TIMEOUT_US=5000000 ./your_app
```

| 结果 | 结论 |
|---|---|
| 成功 | 对端只是慢 |
| `status=10` | 与案例二同根：接收侧饿死 |
| 其它 status | 对端明确拒绝 |
| 仍 -125 | 对端完全不处理 |
| -104 | 对端期间崩溃 |

> 假说：案例二（RNR）和案例四（超时）可能是**同一个根因的两种表现**——
> 对端没 post receive buffer → 发送端陷入 RNR 重试 → 硬件上报得早就成 `status=10`，
> 上报得晚就被 500ms 本地截止抢先成 `-125`。以上表的结果为准。

---

## 7.8 善后三件套（永远划掉）

```
nvme_ctrlr_fail: in failed state.
nvme_fabric_prop_get_cmd_async: Failed to send Property Get fabrics command
nvme_ctrlr_shutdown_async: Failed to read the CC register
```

它们出现在失败之后，是**异步拆机**的必然噪音：shutdown 时要读 CC 寄存器，但 adminq 已经不可用。

**看到它们不要往下推理。**

---

## 7.9 一条时间线模板（自己填）

```
t0  你的代码: spdk_nvme_probe / alloc_io_qpair
t1  建连准备 : device → jetty → socket → hello → segments → submit
t2  等待     : poll CQ
t3  结局     : 成功 / cr_error(status=N) / -104 / -125
t4  拆机     : fatal → -6 → alloc fail / attach_fail
t5  善后     : prop_get / CC register
```

把真实日志按 t0–t5 填进去，位置立刻清楚。

---

## 练习

1. 一屏日志只有 `4505 rc=-1 state=57` 和 `4463`，你要去看哪一轮的日志？为什么？
2. 一屏日志有 `alloc_io_qpair`，但没有 `cr_error` 也没有 `-104`，只有 `-125`。属于哪种形状？
3. 为什么 `fabric.c:628` 出现时，说明"有完成事件"？

---

## 下一讲

**08-cheatsheet.md**：把七讲压成一页。
