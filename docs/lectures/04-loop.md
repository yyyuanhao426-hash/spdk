# 第 4 讲：循环与状态机——"一步一停"的引擎

> **本讲只讲一件事**：attach 不是一条直线调用，而是一个循环反复推着状态机走；`-EAGAIN` 是油门，不是错误。
>
> 前置：01–03。

---

## 一图：三层套娃

```
spdk_nvme_probe
└─ nvme_init_controllers                ← while (true) 住在这里
   └─ spdk_nvme_probe_poll_async        ← 一圈 = 一轮
      └─ for each ctrlr in init_ctrlrs
         └─ nvme_ctrlr_poll_internal    ← "工头"，每轮三件事
            ├─ nvme_ctrlr_process_init  ← 状态机走一步
            ├─ attach_fail_cb           ← 失败出口
            └─ attach_cb                ← 成功出口
```

括号展开（`lib/nvme/nvme.c`）：

```c
nvme_init_controllers(probe_ctx)
{
        while (true) {
                rc = spdk_nvme_probe_poll_async(probe_ctx);   /* 一轮 */
                if (rc != -EAGAIN) return rc;                 /* 不是"没完"就收工 */
        }
}

spdk_nvme_probe_poll_async(probe_ctx)
{
        TAILQ_FOREACH(ctrlr, &probe_ctx->init_ctrlrs, ...)
                nvme_ctrlr_poll_internal(ctrlr, probe_ctx);

        if (两个清单都空) return 0;
        return -EAGAIN;
}

nvme_ctrlr_poll_internal(ctrlr, probe_ctx)      /* 工头 */
{
        rc = nvme_ctrlr_process_init(ctrlr);    /* ① 走一步 */
        if (rc) { attach_fail_cb(...); return; }/* ② 失败出口 */
        if (ctrlr->state != READY) return;      /* ③ 还没好 */
        attach_cb(...);                         /*    成功出口 */
}
```

---

## 4.1 为什么是循环，不是一 call 到底

因为网络要等，而 SPDK 是**轮询模型**：不睡觉、不阻塞，反复问"好了没"。

两种模型的差别：

```
阻塞模型：
  发送 ──▶ [ 线程睡在这里 20ms ] ──▶ 收到 ──▶ 继续

轮询模型（SPDK）：
  发送 ──▶ 返回
  看一眼 ──▶ 没好，返回 -EAGAIN
  看一眼 ──▶ 没好，返回 -EAGAIN
  看一眼 ──▶ 好了 ──▶ 进入下一步
```

所以"初始化"必须被切成很多小步，每步之间允许返回。

---

## 4.2 `-EAGAIN` 的约定

`-EAGAIN` 是 POSIX 的"资源暂时不可用"，SPDK 借来表达"**还没好，再叫我**"。

这个含义不是调用者猜出来的，而是**被调用者明确承诺**的：

```c
/* spdk_nvme_probe_poll_async 的承诺 */
if (清单空了) return 0;        /* 干完了 */
return -EAGAIN;                /* 没干完 */
```

调用者据此写：

```c
if (rc != -EAGAIN) return rc;  /* 只要不是"没完"，就结束 */
```

> **`rc` 是"这一轮的小票"，不是 poll 对象本身。** 每轮换一张。

---

## 4.3 状态机：一步一停

玩具例子：

```c
int tick(void)
{
        switch (state) {
        case STEP_A: 做A(); state = STEP_B; break;
        case STEP_B: 做B(); state = STEP_C; break;
        case DONE:   return 1;
        }
        return 0;       /* 没完，下次再来 */
}
```

真实的 `nvme_ctrlr_process_init` 就是一个大 `switch (ctrlr->state)`，每轮只走一个 case。

---

## 4.4 state 编号表（读 `state: 57` 用）

> 本表按 `lib/nvme/nvme_internal.h` 的 `enum nvme_ctrlr_state` 数出来；
> 日志同时打印名字（如 `(error)`），**以名字为准**，编号会随版本变。

| 值 | 状态 | 值 | 状态 |
|---|---|---|---|
| 0 | INIT_DELAY | 30 | WAIT_FOR_…ZNS |
| **1** | **CONNECT_ADMINQ**（= INIT） | 31 | GET_ZNS_CMD_EFFECTS_LOG |
| **2** | **WAIT_FOR_CONNECT_ADMINQ** | 32 | WAIT_FOR_…ZNS_LOG |
| 3 | READ_VS | 33 | IDENTIFY_IOCS_KV_SPECIFIC |
| 4 | WAIT_FOR_VS | 34 | WAIT_FOR_…KV |
| 5 | READ_CAP | **35** | **SET_NUM_QUEUES** |
| 6 | WAIT_FOR_CAP | 36 | WAIT_FOR_SET_NUM_QUEUES |
| 7 | CHECK_EN | 37 | IDENTIFY_ACTIVE_NS |
| 8 | WAIT_FOR_CC | 38 | WAIT_FOR_IDENTIFY_ACTIVE_NS |
| 9 | DISABLE_WAIT_FOR_READY_1 | 39 | IDENTIFY_NS |
| 10 | WAIT_FOR_CSTS | 40 | WAIT_FOR_IDENTIFY_NS |
| 11 | SET_EN_0 | 41 | IDENTIFY_ID_DESCS |
| 12 | WAIT_FOR_CC | 42 | IDENTIFY_NS_IOCS_SPECIFIC |
| 13 | DISABLE_WAIT_FOR_READY_0 | 43 | WAIT_FOR_…NS_IOCS |
| 14 | WAIT_FOR_CSTS | 44 | WAIT_FOR_ID_DESCS |
| 15 | DISABLED | 45 | SET_SUPPORTED_LOG_PAGES |
| **16** | **ENABLE** | 46 | SET_SUPPORTED_INTEL_LOG_PAGES |
| **17** | **ENABLE_WAIT_FOR_CC** | 47 | WAIT_FOR_…INTEL |
| 18 | ENABLE_WAIT_FOR_READY_1 | 48 | SET_SUPPORTED_FEATURES |
| 19 | WAIT_FOR_CSTS | 49 | SET_HOST_FEATURE |
| 20 | RESET_ADMIN_QUEUE | 50 | WAIT_FOR_SET_HOST_FEATURE |
| **21** | **IDENTIFY** | 51 | SET_DB_BUF_CFG |
| 22 | WAIT_FOR_IDENTIFY | 52 | WAIT_FOR_DB_BUF_CFG |
| 23 | CONFIGURE_AER | 53 | SET_HOST_ID |
| 24 | WAIT_FOR_CONFIGURE_AER | 54 | WAIT_FOR_HOST_ID |
| 25 | SET_KEEP_ALIVE_TIMEOUT | **55** | **TRANSPORT_READY** |
| 26 | WAIT_FOR_KEEP_ALIVE_TIMEOUT | **56** | **READY** |
| 27 | IDENTIFY_IOCS_NVM_SPECIFIC | **57** | **ERROR** |
| 28 | WAIT_FOR_…NVM | **58** | **DISCONNECTED** |
| 29 | IDENTIFY_IOCS_ZNS_SPECIFIC | | |

### 最容易被骗的一点

日志里的 `state:` 是**这一轮结束之后**的值，不是进入时的值。

| 进入时 | case 里做了什么 | 日志打印 |
|---|---|---|
| 1 CONNECT_ADMINQ | 建连失败 → 先设 ERROR 再 return | **57** |
| 16 ENABLE | 先设 17，再调用 enable；失败 return | **17** |
| 21 IDENTIFY | 先设 22，提交失败则设 ERROR | **57** |

所以"看到 57"不等于"它从 57 开始"。

---

## 4.5 两种失败风格

| 风格 | 怎么发生 | 4505 打印的 state | 怎么找到根因 |
|---|---|---|---|
| **同步** | case 自己调用失败，`return rc` | 57（若置了 ERROR）或对应的 WAIT 值 | 就看这一轮的 rc 和上一行日志 |
| **异步** | 命令提交成功，完成里带错误；**回调**置 ERROR | 下一轮打印 57，rc = **-1** | **看上一轮**的日志 |

记住这条判据：

```
4505 rc = -1   → 异步收尾 / 超时，根因在上一轮
4505 rc ≠ -1   → 同步失败，根因就在这一轮
```

---

## 4.6 超时

状态机的每一步都有截止时间：

- `state_timeout_tsc`：当前状态的超时；
- admin 命令的超时；
- Connect 的 500ms（见 05 讲）。

超时会打印：

```
Initialization timed out in state N (...)
```

然后 `return -1`，走失败出口。

---

## 常见误解

| 误解 | 实际 |
|---|---|
| "`-EAGAIN` 是错误" | 是油门：没干完，再叫我 |
| "poll 一次就做完整个初始化" | 一次只走一步 |
| "state 打印的是进入时的状态" | 是**这一轮结束之后**的状态 |
| "日志里同一个函数出现两次 = 重试了两次" | 可能是两层嵌套的 poll 各打了一次 |
| "初始化的每一步都会很快" | 每一步都可能等好几轮 |

---

## 练习

1. 如果 `spdk_nvme_probe_poll_async` 永远返回 `-EAGAIN`，程序会怎样？
2. `4505 rc=-1 state=57` 和 `4505 rc=-5 state=57`，哪一个是"异步失败"？
3. 一个 case 进入时是 16，失败后日志打印 17，为什么？

---

## 下一讲

**05-connect.md**：状态机第 1 步——建连六步，以及 Connect 命令的三种结局。
