# 场景 D — 跨重启持久性验证（SE_Review）

**日期**：2026-10-11
**工具**：`examples/nvme/persist_check`（自写，基于 hello_world 模板）
**验证对象**：SE_Review #4（重连泄漏）降级验证 —— 重启后数据持久性

## 结论

**PASS** — 数据跨 target 重启完整持久，无丢失/损坏。

## 方法与结果

流程：node4（initiator）写入已知模式 → node1 kill nvmf_tgt → restore vfio 盘回内核驱动 → 检查 root 盘 → re-takeover 启动新 target → node4 读回校验。

模式函数：`buf[i] = (seed + i*131 + 17) & 0xff`，`seed = lba*31 + 7`（与 urma_perf preflight 同语义）。

| 轮次 | 范围 | I/O 大小 | I/O 数 | 数据量 | 写入 | 重启后校验 |
|------|------|----------|--------|--------|------|-----------|
| 1 | LBA 0 | 4KB | 8192 | 32MB | OK (8192) | **OK (8192) 全匹配** |
| 2 | LBA 1048576 | 128KB | 8192 | 1GB | OK (8192) | **OK (8192) 全匹配** |

每次重启均按规范：`kill $(cat /var/tmp/nvmf_tgt.pid)` → `target_nvme_takeover.sh -r -y` → `findmnt` 确认 root_src=/dev/mapper/openeuler-root（非 nvme7n1）→ `target_nvme_takeover.sh -d nvme7n1 -y -i 141.61.84.245 -L /home/tong/umdk_gpu_isolated/lib`。

## 工具 bug 修复（重要）

`persist_check.c` 初版 `spdk_nvme_ns_cmd_write/read` 的 `blocks` 参数传 `1`（1 扇区=512B），但 buf 为 4096B → 只写 512B、校验 4096B → 必然 mismatch。**修复**：`seq.blocks = g_io_size / spdk_nvme_ns_get_sector_size(ns)`。修复后写→立即读 64×4KB 全匹配，证明读写路径一致，才进入重启验证。此 bug 与 SPDK/URMA 无关，纯测试工具缺陷。

## 关联结论

- 场景 C 发现的 initiator 断连检测缺失（`nvme_urma.c` 返回 -ECONNRESET 而非 -ENXIO，不调 `nvme_urma_ctrlr_disconnect_qpair`）在本场景不构成阻碍：持久性验证走"重新连接"而非"同一 qpair 重连"。
- GLM #4 修复（epoch + release_transport + DISCONNECTED）在重启路径下正常生效：新连接 epoch 递增，无陈旧 CR 误配。
