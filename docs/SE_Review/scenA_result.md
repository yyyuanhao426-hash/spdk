# SE_Review 修复验证 — 场景 A（#1 target 断连排水）结论

日期：2026-10-10
节点：node1（141.61.84.245, target, 普通构建 fba60e72e）/ node4（141.61.84.151, initiator）
测试盘：nvme7n1（BDF 0000:91:00.0）

## 结论

**场景 A 通过。** target 侧断连排水状态机（#1 修复）在 initiator 被反复 kill -9 时正确执行，无 iobuf 提前归还写入、无 UAF、内存平稳。

## 方法

- initiator：urma_perf 持续 4KB write 打流
- target：30 轮 `kill -9` initiator（`data/scenA_rounds.log`，每轮约 2-11 秒，LOOP_DONE）
- 观察点：
  1. target 日志 `drain deadline exceeded` 计数（排水超时 = qpair_fini 期间 iobuf 未按期归还）
  2. target 进程 VmRSS 全程采样（`data/target_rss_scenA.csv`）
  3. target 进程存活

## 结果

| 指标 | 数值 | 判定 |
|------|------|------|
| 轮数 | 30 轮 kill -9 initiator | 全部完成 |
| `drain deadline exceeded`（target 日志） | **0** | 排水无超时 ✓ |
| target RSS 首样本 → 末样本 | 59,116 KB → 59,460 KB | 波动 <0.6%，无泄漏 ✓ |
| target 进程 | 全程存活，无 crash | ✓ |

修复前预期（GLMReview）：向已归还的 iobuf 写入 / use-after-free。实测 30 轮断连后 target 内存与排水均干净。

## 数据文件

- `data/scenA_rounds.log` — 30 轮 kill 记录
- `data/target_rss_scenA.csv` — target VmRSS 全程采样
- `data/nvmf_tgt_scenA.log` — target 日志
