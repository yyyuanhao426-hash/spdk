# ABI 错位根因图解：同名结构、两套布局

> 一句话：SPDK 和系统 liburma 对 `urma_rjetty_t` 这张"表格"用了**两份不同版本的模板**——
> 写的人按 24 字节 flag 的大模板填，读的人按 4 字节 flag 的小模板取，
> "运输类型（tp_type）"这个格子在小模板的第 40 字节、在大模板的第 64 字节——
> 库去第 40 字节取，取到的是 SPDK 填的"标志位"（0），于是被判成 RTP。

## 内存布局对照（urma_import_jetty 的同一块入参内存）

```
字节偏移    UMDK 头视角（SPDK 二进制按它写入）      CCDK 头视角（系统 liburma 按它读取）
─────────────────────────────────────      ─────────────────────────────────────
@0        jetty_id.eid     df08:0b00          jetty_id.eid     df08:0b00        ✓ 一致
@16       jetty_id.uasid   0                  jetty_id.uasid   0                ✓ 一致
@20       jetty_id.id      8294               jetty_id.id      8294             ✓ 一致
@24       trans_mode       1 (RM)             trans_mode       1 (RM)           ✓ 一致
@28       policy           0                  policy           0                ✓ 一致
@32       type             1 (JETTY)          type             1 (JETTY)        ✓ 一致
@36       (对齐填充)         0
@40       flag (4B)         0           ★★   tp_type          0 = RTP  ✗✗✗   ← 库读这里
@44       flag/user_tag     0                  token            …
@48       flag/user_tag     0
@64       tp_type           1 (CTP)  ★★      （库按 48B 结构，读不到这里）
              ↑                                ↑
        SPDK 把 CTP(1) 写在这               库却在这取 → 取到 0 = RTP
```

## 后果链

```
库读到 tp_type = 0 (RTP)
   ↓
reuse 查找键含 tp_type → 与孤儿条目（CTP）不匹配 → reuse 未命中
   ↓
走管理面查询 GET_TP_LIST → 本机恒败（result=2）
   ↓
import 必败（rc=-5）→ 回环握手失败 rc=-5
```

## 为什么裸探针幸免

裸探针（p228/p231/p232/p248）编译用 `-I/usr/include/ub/umdk/urma`
（CCDK 头）——与系统库**同一份模板**：偏移全部对齐 → tp_type=CTP(1)
正确到达 → reuse 命中 → import 成功。

| 编译头 | tp_type 写入偏移 | 库读取偏移 | 结果 |
|---|---|---|---|
| CCDK 头（裸探针） | 40 | 40 | CTP ✓ 命中 |
| UMDK 头（SPDK 二进制） | 64 | 40 | 读到 flag=0 → RTP ✗ |

## 修复

SPDK 改用 CCDK 头编译（+ 兼容层：尾部追加 app-only `is_gpu_seg`
字段让源码可编译）→ 偏移对齐 → tp_type=CTP 正确到达 → reuse 命中
→ **-M cpu 回环全通（6088/0）**。
