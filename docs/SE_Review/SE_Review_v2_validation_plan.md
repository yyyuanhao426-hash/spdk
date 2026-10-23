# URMA v2 生命周期修复验证计划

## 修复不变量

1. 每个启用完成通知的 URMA WR 在 post 前取得一个 qpair 生命周期引用。
2. CR 被 JFC 取出、保存在 poll 局部数组、转发到 owner thread、执行 handler 的全过程沿用该引用。
3. 关闭位与引用计数位于同一个原子字中。关闭位设置后，新的 post 无法取得引用。
4. jetty/JFR、capsule slot、请求和 iobuf 只能在引用归零且 `working_reqs` 为空后释放。
5. 排空超时只重试 jetty ERROR/flush 并隔离 qpair，不强制归还资源。
6. 半握手 qpair 从 accept 后立即属于 transport，完成、拒绝和 transport destroy 都必须将其移出所属链表。

## 验证矩阵

| 编号 | 覆盖审阅项 | 方法 | 通过条件 |
|---|---|---|---|
| U1 | #1、#2、#3 | 运行 `urma_lifetime_ut`；验证 close 与引用获取的原子互斥、带引用时不能 reopen、归零后才能 reopen | 全部断言通过 |
| U2 | #1、#3 | 两个 poller 共享 JFC；让 B 取出 A 的 CR 后暂停，A 发起断连，随后恢复 B | A 在 B handler 退出前保持 DISCONNECTING；旧 CR 不写入新连接；ASAN/UBSAN 无异常 |
| I1 | #1、#2 | initiator 双线程、至少 8 个 qpair 持续 I/O，循环 kill/restart target 100 次并自动重连 | 每轮旧连接引用归零后才出现 CONNECTED；无二次误断连、UAF、mutex 错误和资源增长 |
| T1 | #3 | target 双 poll group；批量注入同一 qpair 的 capsule 错误 CR 与后续成功 CR | 首条触发关闭后，批内剩余 CR 均能安全处理；qpair 仅在最后 handler 退出后释放 |
| T2 | #3 | kill initiator 100 轮，I/O 混合 H2C/C2H，并启用 ASAN | RX、TX、READ、WRITE 引用最终归零；UAF=0；drain quarantine=0 |
| T3 | #4 | 故障注入使一次 `urma_flush_jetty` 不返回目标 CR，等待超过 10 秒，再恢复完成 | 超时日志包含 `remains quarantined`；超时点不执行 fini、不归还 iobuf；恢复 CR 后正常 fini |
| H1 | #5 | 建立 100 个连接，分别发送 0 字节、半个 header、完整 header 加半个 endpoint，然后删除 transport | 所有 hs poller 被取消、fd/device 引用被回收、pending 列表为空；ASAN 无异常；transport 可再次创建 |
| R1 | 全部 | 重跑既有 A/B/C/D/E/G/H/R 场景 | 原验收指标不退化；NOFbench 吞吐与基线差异不超过既有容差 |

## 关键观测

- initiator 状态顺序必须是 `CONNECTED -> DISCONNECTING -> DISCONNECTED -> CONNECTING -> CONNECTED`，不得在引用非零时进入新一轮连接。
- target 的超时路径允许保留隔离对象；任何超时点都不得出现 `force-releasing`。
- 每轮故障后记录 qpair 引用数、`working_reqs`、RSS、ASAN 报告数和重连次数。
- H1 在 transport destroy 返回后检查 fd 数、URMA device 引用和 poller 数恢复到测试前基线。

## 本地检查

- 用真实 UMDK 头文件分别编译 `lib/nvme/nvme_urma.c` 与 `lib/nvmf/urma.c`。
- 独立最小程序验证关闭门禁、poll 局部 CR 可见性和 drain 后 reopen。
- `git diff --check` 检查补丁格式。

真机测试使用与上一轮相同的 node1 target、node4 initiator 和 `nvme7n1`，先跑 U2/T3/H1 的定向故障，再跑完整回归，便于区分生命周期修复与性能变化。
