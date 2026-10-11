# SE_Review 修复验证 — 场景 C（#4 重连）结论

日期：2026-10-10
节点：node1（141.61.84.245, target, ASAN 构建 fba60e72e）/ node4（141.61.84.151, initiator）
测试盘：nvme7n1（BDF 0000:91:00.0）

## 结论

**场景 C 未通过（阻塞），原因是 URMA initiator transport 断连检测缺失，GLM #4 修复（epoch/teardown）无触发路径。** 按用户指示记录结论并跳过场景 C。

## 执行记录

- 目标：node4 跑 SPDK `examples/nvme/reconnect` 例程，node1 循环 kill/restart target（计划 10 轮），验证 initiator 重连成功、epoch 丢弃、无泄漏。
- 实际执行：10 轮 kill/restart 循环全部完成（`/tmp/se_c/rounds.log`），但：
  1. 第一轮验证脚本未包含 `-i 141.61.84.245`（自动探测 listener 选错网卡：veth 10.12.1.178 而非 enp165s0 141.61.84.245），reconnect 连不上。
  2. 手动修复 listener 后复测，reconnect 例程在 target 断连时报错退出：
     ```
     nvme_qpair.c:914: ... CQ transport error -104 (Connection reset by peer)
     Received an unknown error processing completions.   <- reconnect.c:177, exit(1)
     ```
- 下载数据：`data/scenC_reconnect.log`（node4 reconnect 日志，含上述 -104 退出）、`data/scenC_rounds.log`（node1 重启循环记录）。

## 根因分析（代码证据）

对比 target 侧与 initiator 侧对 TCP 控制面断连（recv==0）的处理：

| 环节 | target 侧 `lib/nvmf/urma.c` | initiator 侧 `lib/nvme/nvme_urma.c` |
|------|------------------------------|--------------------------------------|
| lifetime socket 检查 | `nvmf_urma_check_lifetime_socket`（1937） | `nvme_urma_check_lifetime_socket`（1147） |
| EOF 返回值 | `-ECONNRESET`（1948） | `-ECONNRESET`（1167） |
| 断连后的动作 | **调用** `nvmf_urma_disconnect_qpair`（2210，排水状态机 + 资源回收） | **不调用**，`nvme_urma_qpair_process_completions` 拿到 rc<0 直接 return（1185） |
| 返回错误码 | 触发 qpair 断开流程 | `-104`，非标准 `-ENXIO` |

标准语义对照（SPDK TCP transport，`lib/nvme/nvme_tcp.c`）：
- `nvme_tcp_qpair_process_completions` 失败路径（`fail:` 标签）设置 `transport_failure_reason` → `nvme_ctrlr_disconnect_qpair()` → `return -ENXIO`（nvme_tcp.c:2438-2441）。
- 上层 `spdk_nvme_qpair_process_completions`（nvme_qpair.c:869）据此返回 `-ENXIO`，驱动 reconnect 例程 `nvme_check_io`（reconnect.c:171）走 `spdk_nvme_ctrlr_reconnect_io_qpair` 重连。

URMA initiator 侧问题：
1. `nvme_urma_qpair_process_completions` 在 TCP 断连（recv==0）分支返回 `-ECONNRESET`（nvme_urma.c:1215），lifetime socket 检查也返回 `-ECONNRESET`（1167）。
2. **从不调用** `nvme_urma_ctrlr_disconnect_qpair`（nvme_urma.c:1436，即 GLM #4 修复实现的 epoch++ / release_transport / DISCONNECTED 状态机）。该函数仅有定义和 ops 表注册，无断连路径调用点（grep 确认：1436 定义、1615 ops 注册，无其他调用）。
3. 结果：qpair 永远停在 ENABLED，epoch 不变，标准重连（reconnect 例程、`spdk_nvme_ctrlr_reconnect_io_qpair`）不可达。

## 影响

- **GLM #4 修复无法被验证**：epoch 丢弃旧 CR、连接级 teardown 逻辑在 TCP 断连路径上永远不执行。该修复只在「上层框架主动调用 disconnect_qpair」（如 ctrlr_reset / delete qpair）时才会生效，而 URMA transport 自身的断连检测从未触发它。
- **URMA transport 独立缺陷**：initiator 侧断连检测不完整，对称实现存在于 target 侧（lib/nvmf/urma.c:2203-2210），initiator 侧缺失。

## 修复建议（未执行）

在 `nvme_urma_qpair_process_completions` 的断连分支（recv==0 / lifetime socket EOF / recv 错误）调用 `nvme_urma_ctrlr_disconnect_qpair(qpair->ctrlr, qpair)` 并返回 `-ENXIO`（对齐 nvme_tcp.c 的 fail: 路径与 target 侧语义），使 GLM #4 的 epoch/teardown 状态机可被标准重连路径触发。

## 遗留

- 场景 C 数据：10 轮循环日志（listener 选错网卡，无效）、单轮手动复现日志（-104 退出，有效证据）。
- 后续若需验证 #4，先实施上述修复并重新编译 initiator（及 ASAN worktree）后再跑。
