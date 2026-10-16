# 今日进展：NDS 直连（NPU Direct SSD）

## 一、重大突破：回环 import 失败根因坐实并修复 ✅

困扰 20+ 批次的「-M cpu 回环握手失败（rc=-5）」根因落定：**编译期头文件与运行期库的 ABI 错位**——`urma_import_jetty_flag_t`（union）CCDK 版 4 字节 / git-UMDK 版 24 字节，导致 `urma_rjetty_t.tp_type` 偏移 40 vs 64。SPDK 二进制按 UMDK 头编译、按 CCDK ABI 的系统库运行 → 库读到错位的 tp_type → 内核视为 RTP(0) → reuse 键不匹配 → 管理面查询（本机恒败）→ import 必败。

**修复**：CCDK 头 + 尾部 app-only 字段的兼容层重编 → **-M cpu 回环全链路打通（completed=6088 errors=0）**，多轮回归稳定。

## 二、-M npu 数据面：失败定性 + 系统性排查 ✅（进行中）

import 修复后 -M npu 握手已通（RA/HCCP 注册 ✅、direct route 段交换 ✅、双方 jetty import reuse 命中 ✅），但数据面首个操作失败：**target 对 HBM 段的远端读无应答，重传 11 次超限（status=9 = ACK_TIMEOUT），SSD 回读证实 DMA 未发生**。

今日系统性排除了 9 个假设（每批带判决实验）：

| # | 已证伪假设 | 判决手段 |
|---|-----------|---------|
| 1 | 库版本（换 UMDK 库） | 该库在新内核根本不可用 |
| 2 | JFC 数量 | env 生效仍败 |
| 3 | context 序号 | 裸侧 1/2/3 个 context 全成功 |
| 4 | import_flag 参数 | 对齐 P2-19 实测值无效 |
| 5 | TP 域（liburma jetty 不能读 HBM） | 裸探针同款 jetty 读 HBM 成功 |
| 6 | HBM 侧 RA 栈完整度 | 切换 libnds 完整栈仍败 |
| 7 | EAL/env/DPDK | 裸+env 双对照 |
| 8 | 第二处 ABI 错位 | 全类型 offsetof 审计干净 |
| 9 | 设备选择/对端健康度 | 跨设备+健康对端对照 |

## 三、当前根因假设与修复（已编码，判决批已下发）⏳

综合全部数据，唯一能与所有现象自洽的解释：**RA lmem 注册的 HBM 段，远端可达性绑定在 RA 域的 TP/jetty 上——数据面 WR 的远端腿（tjetty）必须指向 RA jetty**。裸探针的成功形态恰是「tjetty = import 的 RA jetty」，SPDK 失败形态是「tjetty = 对端 SPDK jetty」。

**修复已编码**：initiator 在控制面交换中发布 RA jetty 坐标（新增 provider op + HELLO 携带），target 代码零改动。判决批（P2-52）已下发：数据面通过 = **V2 数据判决完成，直连链路全程打通**。

## 四、状态总览

| 项 | 状态 |
|---|---|
| import ABI 根因修复 + -M cpu 全通 | ✅ |
| -M npu 握手/注册/段交换/jetty import | ✅ |
| -M npu 数据面（远端读 HBM） | ⏳ 修复已编码，判决批待回执 |
| V3 性能对比 | 排队（V2 后） |
