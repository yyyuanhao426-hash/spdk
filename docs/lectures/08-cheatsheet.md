# 第 8 讲：一页速查卡

> 贴在显示器旁。忘记细节时先看这里，再回对应讲义。

---

## 一、六幕全景

```
[1] probe         填表登记：ctrlr(state=1)、记回调、挂待办        不发网络包
      │
[2] while poll    工头每轮：process_init 走一步 → 成功/失败出口
      │
[3] 建连 admin    设备 → jetty → socket → hello → 段注册 → Connect   同步失败在这
      │
[4] 等盖章        收 CR：成功 / 错误(-5) / 断开(-104) / 超时(-125)
      │
[5] 自检+读料     VS/CAP/EN/IDENTIFY/NS/queues → READY → attach_cb
      │
[6] 用起来        alloc io qpair → 再走一遍 [3][4]（qid≥1，失败不判死）
      │
[拆] 善后         fail → detach_async → prop_get/CC 失败（噪音）
```

---

## 二、三层调用结构

```
spdk_nvme_probe
└─ nvme_init_controllers            while (true) { poll_async }
   └─ spdk_nvme_probe_poll_async    一轮
      └─ nvme_ctrlr_poll_internal   工头
         ├─ nvme_ctrlr_process_init 状态机走一步
         ├─ attach_fail_cb          失败出口
         └─ attach_cb               成功出口
```

---

## 三、状态编号（关键值）

```
 1 CONNECT_ADMINQ        21 IDENTIFY           56 READY
 2 WAIT_FOR_CONNECT      35 SET_NUM_QUEUES     57 ERROR
16 ENABLE                55 TRANSPORT_READY    58 DISCONNECTED
17 ENABLE_WAIT_FOR_CC
```

> `4505` 打印的是**这一轮结束之后**的 state：
> 进入 1、失败后打印 57；进入 16、失败后打印 17。

---

## 四、rc 五类

| 值 | 含义 | 是错误吗 |
|---|---|---|
| 0 | 成功 | 否 |
| 负 errno | 真实错误 | 是 |
| -EAGAIN (-11) | 轮询层：没干完；阻塞层：5s 超时 | 分情况 |
| -ENXIO (-6) | 通道已拆（信号） | 信号，不是根因 |
| -1 | 状态机到终点（不是 errno） | 是 |
| -125 | 我方截止时间到了 | 是，但不是对端报的 |

常用：-2 裸 errno、-5 垃圾桶、-19 wrapper 的 ENODEV、-104 对端断开、-110/111 TCP 超时/拒绝。

---

## 五、Connect 的三种结局

| 形状 | 日志指纹 | rc | 有 status 吗 |
|---|---|---|---|
| ① CR 报错 | `stage=cr_error status=N` → `err=-5` → `fatal -5` | -5 | ✅ |
| ② 对端断开 | `fatal rc=-104` | -104 | ❌ |
| ③ 超时 | `fabric:617 rc -125`，无 628 | -125 | ❌（没有 CR） |

补充：
- `628 sct 0 sc 8` = 本地中止，不是对端拒绝。
- `617` 后面跟 `628` = 有完成事件；不跟 = 超时。

---

## 六、决策树

```
有 alloc_io_qpair？
├─ 有 → 已 READY
│        ├─ cr_error？ → 形状① 读 status
│        ├─ fatal -104？ → 形状②
│        └─ 617 rc -125？ → 形状③
└─ 无 → 初始化中
         ├─ 4505 rc=-1 → 异步收尾，看上一轮
         ├─ 4505 state=57 rc≠-1
         │     ├─ 有 4447 → TRANSPORT_READY（URMA 不该出现）
         │     └─ 无 4447 → CONNECT_ADMINQ / 提交 helper
         └─ 4505 state=4/6/8/12/17 → 提交 Property 失败
```

---

## 七、读日志五问

1. 谁打的？（文件→函数→哪一幕）
2. 根因还是翻译？（找 `cr_error` / `fatal` / `4505`）
3. rc 属于哪类？（-1 / -125 / -6 / 其它）
4. 缺了什么？（没有 cr_error = 没有 CR；没有 alloc = 未 READY；没有 628 = 超时）
5. 哪些是善后？（failed state / prop_get / CC register → 划掉）

---

## 八、六句口诀

1. probe 只填表，返回 0 ≠ 连上。
2. 循环是引擎，`-EAGAIN` 是油门。
3. attach = probe→READY 的全过程，不是 READY 之后。
4. 失败四张脸：同步 rc、CR 错误、断开、超时。
5. 越往上翻译越粗，根因在最下面。
6. 善后日志出现得最晚，但从来不是线索。

---

## 九、常用环境变量

| 变量 | 作用 |
|---|---|
| `SPDK_URMA_TRACE=1` | 打开 URMA 追踪 |
| `MC_NVME_FABRICS_CONNECT_TIMEOUT_US` | Connect 超时（默认 500000 = 500ms） |
| `URMA_PERF_REGION_REG` | urma_perf 整池注册（**只要存在就开启**，`=0` 也算开） |
| `SPDK_URMA_DEV_NAME` | 指定 URMA 设备名（如 bonding_dev_0） |

---

## 十、最小验证实验

怀疑"对端慢"而不是"对端坏"时：

```bash
MC_NVME_FABRICS_CONNECT_TIMEOUT_US=5000000 ./your_app
```

结果 → 结论：

```
成功        → 只是慢
status=10   → 接收侧饿死（RNR）
其它 status → 对端明确拒绝
仍 -125     → 对端完全不处理
-104        → 对端期间崩溃
```

---

## 十一、对照表：日志行 → 讲义

| 日志 | 回到哪一讲 |
|---|---|
| `spdk_nvme_probe`、`attach_fail_cb`、`rc=-19` | 03 |
| `-EAGAIN`、`state: 57`、`Ctrlr is in error state` | 04 |
| `Connect command failed`、`sct 0 sc 8`、`-125` | 05 |
| `-2`、`-5`、`-6`、`Operation not permitted` | 06 |
| 四个案例、决策树 | 07 |
