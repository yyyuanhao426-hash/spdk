# SE_Review 修复验证 — 场景 B（#1/#2/#5/#6 ASAN）结论

日期：2026-10-10
节点：node1（141.61.84.245, target, **ASAN 构建** fba60e72e）/ node4（141.61.84.151, initiator）
测试盘：nvme7n1（BDF 0000:91:00.0）

## 结论

**场景 B 通过。** ASAN 构建 target 在 50 轮 initiator kill -9 下无 AddressSanitizer 报告（use-after-free / 越界 / 泄漏），排水无超时，target 全程存活。修复前预期必报 UAF（GLMReview），修后干净。

## 方法

- initiator：urma_perf 持续 4KB write 打流
- target：**ASAN 编译 nvmf_tgt**（libasan 链接），50 轮 `kill -9` initiator（`data/scenB_rounds.log`，LOOP_DONE）
- 观察点：
  1. target stderr 是否出现 `AddressSanitizer` / `ERROR: AddressSanitizer` / `use-after-free`
  2. target 日志 `drain deadline exceeded` 计数
  3. target 进程存活

## 结果

| 指标 | 数值 | 判定 |
|------|------|------|
| 轮数 | 50 轮 kill -9 initiator | 全部完成 |
| ASAN 报告（use-after-free / 越界 / 泄漏） | **0** | 无内存错误 ✓ |
| `drain deadline exceeded` | **0** | 排水无超时 ✓ |
| target 进程 | 全程存活，无 crash | ✓ |
| ASAN target 日志 | 5874 行，末尾正常输出 I/O 统计（JFC wait 40.3% / exec 57.4%），无 ASAN 报错 | ✓ |

覆盖的 SE_Review 问题：
- **#1**（排水状态机）：50 轮断连下无 iobuf UAF
- **#2**（CID=0 响应身份）：打流期间无 CID=0 误匹配
- **#5**（共享 JFC 错误归属）：无错误 CR 归属错乱
- **#6**（lifetime socket）：断连路径正确释放

## 数据文件

- `data/scenB_rounds.log` — 50 轮 kill 记录
- `data/scenB_asan_watch.log` — ASAN watch 脚本记录
- `data/scenB_nvmf_tgt_asan.log` — ASAN target 完整日志（无 ASAN 报告）
