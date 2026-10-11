# 场景 E — DoS 半开连接（SE_Review #3）

**日期**：2026-10-11
**验证对象**：fba60e72e 提交 —— target 握手非阻塞状态机 + 5s 期限（`NVMF_URMA_HANDSHAKE_TIMEOUT_S=5`，`NVMF_URMA_HANDSHAKE_POLL_US=1000`）

## 结论

**PASS** — 并发 5 个只连接不发 HELLO 的客户端无法卡住 accept 线程，半开连接 5s 后被超时关闭，期间正常 I/O 服务不受影响。

## 方法与结果

node1 上并发发起 5 个 nc 连接 `141.61.84.245:4420`，不发 HELLO，挂 8 秒；同时 node4 跑 persist_check 正常 I/O（1024×4KB write+verify）。

| 观测项 | 结果 |
|--------|------|
| 5 个半开连接处理 | 全部在 5s deadline 后关闭（日志 5 条 `accept: handshake timeout`，09:50:35.003→.010） |
| DoS 期间正常 I/O | write OK (1024) + verify read-back matched OK (1024) |
| target 存活 | PID 2176322 全程存活，listener 4420 正常 |
| `drain deadline exceeded` | 0 |

**修复前预期**：accept poller 同步阻塞 recv，单个半开连接永久卡死线程，后续连接全无响应。修复后非阻塞增量握手（`nvmf_urma_handshake_progress` 状态机）+ 1ms poller + 5s 期限，EAGAIN 留给状态机推进。
