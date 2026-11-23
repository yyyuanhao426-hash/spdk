# 第 2 讲：函数指针与回调——把电话留给 SPDK

> **本讲只讲一件事**：函数名在 C 里是地址，可以当参数传；`probe_cb` / `attach_cb` 就是这样"留电话"。
>
> 前置：第 1 讲；见过 `struct`、指针。

---

## 一图

```
普通调用：  你 ──── call ────▶ 别人            （你主动）

回调：      你 ── 先交出函数地址 ──▶ 别人
                别人 ── 在某个时刻 call 回来 ──▶ 你的函数
```

---

## 2.1 函数也有地址

```c
void my_handler(int x) { /* ... */ }

void (*p)(int) = my_handler;   /* p 里存的就是函数的地址 */
p(42);                          /* 等价于 my_handler(42) */
```

`&my_handler` 和 `my_handler` 在这里是同一个东西。

---

## 2.2 看懂 `typedef`：去掉 typedef，剩下的就是变量名

```c
typedef void (*spdk_nvme_attach_cb)(void *cb_ctx,
                                    const struct spdk_nvme_transport_id *trid,
                                    struct spdk_nvme_ctrlr *ctrlr,
                                    const struct spdk_nvme_ctrlr_opts *opts);
```

**读法**：先在心里擦掉 `typedef`，读成一个变量声明：

```c
void (*spdk_nvme_attach_cb)(void *cb_ctx, ...);
```

> "声明一个变量 `spdk_nvme_attach_cb`，类型是**指向函数的指针**；该函数返回 `void`，参数是括号里那些。"

把 `typedef` 加回去，这个变量名就升级成**类型名**。别名就是括号里那个名字。

### 为什么必须有括号

```c
void  *f(int);     /* f 是函数，返回 void*        */
void (*f)(int);    /* f 是指针，指向一个函数       */
```

`()` 的优先级高于 `*`。括号的唯一作用是让 `*` 先和名字结合。

```
void (*f)(int)
      │  │
      │  └── f 先和 * 结合 → f 是指针
      └───── 指向"返回 void、参数 int"的函数
```

> 参数名（`cb_ctx`、`trid`…）只是文档，不影响类型；写成匿名参数也合法。

---

## 2.3 SPDK 的四个回调

| 回调 | 什么时候被调 | 参数里带什么 | 你该做什么 |
|---|---|---|---|
| `probe_cb` | 发现一个候选设备 | `trid`、可改的 `opts` | 说"要/不要"，改参数 |
| `attach_cb` | 该设备**初始化成功** | `ctrlr`（钥匙） | 收下 ctrlr |
| `attach_fail_cb` | 该设备**初始化失败** | `rc`（失败码） | 记日志 / 重试 |
| `remove_cb` | 设备被热拔 | `ctrlr` | 清理 |

头文件里的类型（`include/spdk/nvme.h`）：

```c
typedef bool (*spdk_nvme_probe_cb)(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
                                   struct spdk_nvme_ctrlr_opts *opts);
typedef void (*spdk_nvme_attach_cb)(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
                                    struct spdk_nvme_ctrlr *ctrlr,
                                    const struct spdk_nvme_ctrlr_opts *opts);
typedef void (*spdk_nvme_attach_fail_cb)(void *cb_ctx, const struct spdk_nvme_transport_id *trid,
                                         int rc);
typedef void (*spdk_nvme_remove_cb)(void *cb_ctx, struct spdk_nvme_ctrlr *ctrlr);
```

---

## 2.4 `cb_ctx`：存包处的号码牌

`cb_ctx` 是 `void *`：**SPDK 不知道里面是什么，也不需要知道**。它只负责保管，回调时原样还给你。

```cpp
/* 你交出：把 info 一起交出去 */
spdk_nvme_probe(trid, (void *)info, probe_cb, attach_cb, NULL);
/*                  ^^^^^^^^^^ cb_ctx */

/* 你取回：在回调里强转回来 */
auto attach_cb = [](void *cb_ctx, ..., struct spdk_nvme_ctrlr *ctrlr, ...) {
    ctrlr_info *info = (ctrlr_info *)cb_ctx;   /* ← 取包 */
    info->ctrlr = ctrlr;                       /* ← 收钥匙 */
};
```

比喻：存包处。你把包存进去，SPDK 只保管，不打开。

---

## 2.5 "注册回调"的字面实现

`nvme_probe_ctx_init()`（`lib/nvme/nvme.c`）就是一段抄地址的代码：

```c
probe_ctx->cb_ctx    = cb_ctx;      /* 存你的袋子 */
probe_ctx->probe_cb  = probe_cb;    /* 存函数地址 */
probe_ctx->attach_cb = attach_cb;
probe_ctx->attach_fail_cb = attach_fail_cb ? attach_fail_cb
                                           : nvme_dummy_attach_fail_cb;  /* 默认值 */
```

**回调不玄**：注册 = 把地址抄进结构体；触发 = 从结构体里取出来调用。

---

## 2.6 没留电话会怎样

你传了 `attach_fail_cb = NULL`，SPDK 就用自己的默认实现：

```c
/* lib/nvme/nvme.c */
static void
nvme_dummy_attach_fail_cb(void *cb_ctx, const struct spdk_nvme_transport_id *trid, int rc)
{
        SPDK_ERRLOG("Failed to attach nvme ctrlr: ... %s\n", spdk_strerror(-rc));
}
```

日志里那行 `Failed to attach nvme ctrlr: ...` 就是它打的。**它把 `-rc` 当 errno 翻译**，所以 `rc=-1` 会显示成 "Operation not permitted"（EPERM）——这是误读陷阱之一（见 06 讲）。

---

## 常见误解

| 误解 | 实际 |
|---|---|
| "回调 = 再调用一次" | 回调强调**方向**：别人在别的时刻调你 |
| "回调函数我得自己调" | 不用。你只提供地址，调用权在对方 |
| "回调会在我方便的时机执行" | 不会。执行时你正站在**别人的栈帧**里 |
| "被回调的是设备" | 永远是**函数**；设备只是参数 |
| "`probe_cb` 是 SPDK 的" | 是你写的；SPDK 只是存了地址 |

---

## 2.7 为什么回调会带来"重入"问题

因为回调执行时，调用栈可能是：

```
main → spdk_nvme_probe → 循环 poll → 收 CR → 完成处理 → 你的 callback
```

所以代码里会出现这样的守卫：

```c
/* nvme_urma_qpair_process_completions 内 */
if (qpair->in_connect_poll) {
        return completed;      /* 防止 poll → 回调 → 又 poll 的递归 */
}
```

这不是多此一举，而是回调模型自带的坑。

---

## 练习

1. 写出一个 `typedef`：函数指针 `my_cmp`，指向"接受两个 `const void *`、返回 `int`"的函数。
2. 在 `spdk_nvme_probe(trid, info, probe_cb, attach_cb, NULL)` 这一行里，分别指出：哪个是函数、哪个是袋子、哪个是默认值。
3. 为什么 `attach_cb` 收到了 `ctrlr`，而 `attach_fail_cb` 只收到 `rc`？

---

## 下一讲

**03-probe.md**：probe 做了什么、为什么"返回 0 不等于连上"。
