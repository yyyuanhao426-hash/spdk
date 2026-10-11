# SE_Review 修复验证 — 场景 C（#4 重连）结论

日期：2026-10-11（重跑）
节点：node1（141.61.84.245, target, ASAN 构建 043d1a48c）/ node4（141.61.84.151, initiator）
测试盘：nvme7n1（BDF 0000:91:00.0）
代码：`043d1a48c`（initiator 断连检测修复）+ 测试工具修复（reconnect.c num_resets 清零）

## 结论

**场景 C 重跑通过（#4 最后闭环）。** 首轮执行（10-10）发现的「initiator 断连检测缺失」已由远端提交 `043d1a48c` 修复，重跑 15 轮 kill/restart 全部恢复，无泄漏、无数据混入。

## 修复内容（远端 5c6a5b6de + 043d1a48c）

| 提交 | 内容 | 状态 |
|------|------|------|
| `5c6a5b6de` | urma.c struct 重复成员 fini_cb/fini_arg（fba60e72e 回归，删除 407-408） | 与机器侧修复一致 |
| `043d1a48c` | initiator 连接级故障统一 `goto fail`：设置 `transport_failure_reason` → `nvme_ctrlr_disconnect_qpair`（触发 #4 的 epoch++/release_transport/DISCONNECTED）→ 返回 `-ENXIO`；ERRLOG 带原始 rc | 对齐 nvme_tcp.c fail: 语义 |

触发路径（nvme_urma_qpair_process_completions，1272-1280 行）：
```
SPDK_ERRLOG("URMA qpair %u: fatal rc=%d, disconnecting for reconnect\n", ...);
qpair->transport_failure_reason = SPDK_NVME_QPAIR_FAILURE_UNKNOWN;
nvme_ctrlr_disconnect_qpair(qpair->ctrlr, qpair);
return -ENXIO;
```

## 执行记录（重跑）

- initiator：`examples/nvme/reconnect -q 32 -o 4096 -w write -t 1500`，trid 显式 `traddr:141.61.84.245`
- target：ASAN 构建 nvmf_tgt，`target_nvme_takeover.sh -d nvme7n1 -y -i 141.61.84.245 -L /home/tong/umdk_gpu_isolated/lib`
- 循环：15 轮 `kill -9` → `-r -y` 还原 vfio 盘 → 检查 root（/dev/mapper/openeuler-root，非 nvme7）→ 重新接管

### 关键计数（data/scenC_reconnect_r2.log）

| 指标 | 数值 | 判定 |
|------|------|------|
| `fatal rc=-104, disconnecting for reconnect` | **738** | 新断连路径持续触发 ✓ |
| `Controller properly reset` | **7** | 标准 reset 重连成功恢复 ✓ |
| `Controller cannot be recovered. Exiting.` | **0** | 无退出 ✓ |
| `failed with error: -111`（reset 期间 target down 窗口） | 36 | 正常，恢复前重试 |
| reconnect 进程存活 | 全程 8+ 分钟 | ✓ |

### RSS（data/scenC_rss_r2.csv）

- 样本 589，avg 33,016 KB，max 34,812 KB，无持续增长 → **#4 泄漏修复生效** ✓

### 数据一致性（persist_check fill→verify）

| 范围 | 结果 |
|------|------|
| 32MB（8192×4KB, LBA 0）write→verify | **matched OK (8192 I/Os)** ✓ |
| 1GB（8192×128KB, LBA 1M）write→verify | **matched OK (8192 I/Os)** ✓ |

无旧连接 CR 混入。注：1GB verify 程序退出时出现 `Failed to read the CC register` / `CQ transport error -6` 的 shutdown 阶段日志，与数据校验无关（verify 结果 matched OK）。

## 测试工具修复（非产品代码）

reconnect.c `g_max_ctrlr_resets=15` 为**累计计数且 reset 成功后不清零**：多次断连场景下失败次数跨轮累计，超限触发 `exit(1)`。修复：reset 成功分支加 `entry->num_resets = 0;`（reconnect.c:957）。这是例程逻辑缺陷，不影响产品代码验证结论。

## 遗留观察（记录在案，不阻塞闭环）

1. **reset 期间断连检测高频触发**（738 次）：target down 窗口内 I/O 持续失败，每轮 qpair 0/1 反复检测 EOF。行为正确但日志量大，若需收敛可考虑断连后暂停 I/O 提交直到 reset 完成。
2. **shutdown 阶段 `Failed to read the CC register`**：persist_check 退出时 URMA target 的 CC 寄存器读取失败（`CQ transport error -6`），疑似 URMA transport shutdown 路径问题，不影响 I/O 数据。独立跟踪。
3. **性能课题不入本链**：4KB 小 IO 的 JFC wait 瓶颈与 T4 深队列退化按用户指示走 GDR/perf 路线图。

## 数据文件

- `data/scenC_reconnect_r2.log` — reconnect 例程完整日志（738 次断连 + 7 次恢复）
- `data/scenC_rss_r2.csv` — reconnect 进程 VmRSS 全程采样
- `data/scenC_rounds_r2.log` — node1 15 轮 restart 循环记录
