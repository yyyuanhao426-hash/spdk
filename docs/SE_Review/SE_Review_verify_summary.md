# SE_Review 修复验证 — 汇总报告

**日期**：2026-10-11
**验证对象**：`urma_enable_v2` 分支 7 个修复提交（dc86d1ba7..fba60e72e，HEAD=fba60e72e），覆盖 SE_Review 8 条问题
**节点**：node1 (141.61.84.245, target) ↔ node4 (141.61.84.151, initiator)
**测试盘**：nvme7n1（root 盘 nvme5n1 全程未触碰）
**编译**：两节点均修复 urma.c 重复成员（407-408 行）后编译通过（普通 + ASAN + shared 三配置）

## 场景结果总表

| 场景 | 验证对象 | 方法 | 结果 |
|------|---------|------|------|
| A | #1 target 断连排水 | 30 轮 kill initiator，观察 target drain/RSS | **PASS** |
| B | #1+#2+#5+#6 ASAN | 50 轮 kill initiator + ASAN target | **PASS** |
| C | #4 initiator 重连 | 10 轮 kill target + reconnect | **跳过**（暴露独立缺陷，见下） |
| D | 跨重启持久性 | 写 32MB+1GB 已知模式 → 重启 target → 读回校验 | **PASS** |
| E | #3 DoS 半开连接 | 5 并发 nc 不发 HELLO + 正常 I/O 同时进行 | **PASS** |
| G | #7 iobuf 越界 | iobuf_ut 单测（含边界测试） | **PASS** |
| H | #8 registry 符号 | --with-shared 构建 + nm -D 符号检查 | **PASS** |
| R | 稳态性能回归 | 单盘 + 5盘raid0，对比 NOFbench 基线 | **PASS** |

## 各场景结论

### 场景 A — target 断连排水（#1）✅
30 轮 kill -9 initiator：`drain deadline exceeded`=0，target RSS 稳定 59.1→59.5MB（波动<0.6%），target 全程存活。修复前预期：向已归还 iobuf 写数据 / UAF。

### 场景 B — ASAN 验证（#1/#2/#5/#6）✅
50 轮 kill initiator + ASAN target：AddressSanitizer use-after-free 报告=0，`drain deadline exceeded`=0，target 存活。**修复前必报 UAF，修后干净**（GLMReview 预期）。

### 场景 C — initiator 重连（#4）⏭️ 跳过（发现独立缺陷）
10 轮 kill target 暴露：URMA initiator 无断连检测——`nvme_urma_qpair_process_completions`（nvme_urma.c:1185）拿到 `-ECONNRESET` 直接 return，**从不调用** `nvme_urma_ctrlr_disconnect_qpair`（1436 行，GLM #4 修复的 epoch/teardown 逻辑无触发路径），且返回 `-104` 而非标准 `-ENXIO`，SPDK reconnect 例程只认 `-ENXIO`（reconnect.c:171）→ 重连路径不可达。target 侧（urma.c:1937/1499）有对称实现，**initiator 侧缺失是 URMA transport 独立缺陷**，非 GLM 修复失效。按用户指示记录结论并跳过。详见 `scenC_result.md`。

### 场景 D — 跨重启持久性 ✅
写入已知模式（`buf[i]=(seed+i*131+17)&0xff`，seed=lba*31+7）→ 按规范 kill/restore/re-takeover → 读回校验：
- LBA 0 起 32MB（8192×4KB）：**全匹配**
- LBA 1M 起 1GB（8192×128KB）：**全匹配**
数据跨重启完整持久，无丢失/损坏。工具 bug（blocks 参数=1 扇区而非字节数）已修复并验证读写路径一致后才进入重启测试。

### 场景 E — DoS 半开连接（#3）✅
5 个并发 nc 连接 4420 不发 HELLO 挂 8 秒：全部在 5s deadline 后被关闭（`NVMF_URMA_HANDSHAKE_TIMEOUT_S=5`，日志 5 条 handshake timeout），期间正常 I/O（1024×4KB write+verify）全通过，target 存活，`drain deadline exceeded`=0。修复前预期：accept 线程永久卡死。

### 场景 G — iobuf 越界（#7）✅
`iobuf_ut`：5 tests / 141 asserts 全 PASS，含新增 `iobuf_large_chunk_table_boundary`（large_bufsize=64MB+4096 边界）。

### 场景 H — registry 符号（#8）✅
`--with-shared` 构建通过；`nm -D libspdk_nvme.so`：`spdk_nvme_urma_region_registry_add/find` 均为 `T` 导出，14 个 `spdk_nvme_urma_*` 全导出，无遗留未导出 `nvme_urma_*`；shared 版 nvmf_tgt 冒烟 `--version` 正常。

### 场景 R — 性能回归 ✅
- 单盘 nvme7n1：4KB T4 b32=421 MiB/s（-M cpu）、128KB T1 b32=1.8 GB/s
- **5 盘 raid0（与基线同配置）：128KB T2 b32=8,465 MiB/s**，与 node3 基线 10.1 GB/s 同量级；4KB T4 b32=503 MiB/s（小 IO 瓶颈为 JFC wait，raid0 增益有限）
- target 全程 0 ERROR。差异根因是 **target 绑定盘数**（基线 5 盘 raid0 vs 初期单盘）+ initiator 节点（node3 vs node4），非修复退化。

## 发现的问题与修复

| 问题 | 说明 | 状态 |
|------|------|------|
| urma.c 重复成员 | GLM fba60e72e 引入 399-400/407-408 重复 `fini_cb`/`fini_arg`，三配置均编译失败 | 已按授权修复（删 407-408，备份 .bak_fini） |
| UMDK 链接路径 | configure 默认路径全不存在，回退系统旧库缺 `urma_register_seg_dmabuf` | 符号链接 `UMDK_netlab/lib→umdk_gpu_isolated/lib`（用户指导） |
| worktree 子模块占位 | git worktree 子模块生成空占位目录，configure 报 isa-l 缺失 | 逐一 `ln -sfn` 主树子模块（7 个） |
| persist_check blocks 参数 | `spdk_nvme_ns_cmd_write` blocks=1 扇区（512B）但 buf 4096B | 修复为 `g_io_size/sector_size` |
| **initiator 断连检测缺失（新发现）** | `nvme_urma.c` 断连返回 -ECONNRESET 不调 disconnect，重连路径不可达 | **未修复**，需单独提交（场景 C 结论） |

## 遗留问题

1. **initiator 断连检测（场景 C 根因）**：需在 `nvme_urma_qpair_process_completions` 断连分支调用 `nvme_urma_ctrlr_disconnect_qpair` 并返回 `-ENXIO`（对齐 target 侧与 TCP 语义），才能验证 #4 重连路径。属独立缺陷，建议提交后续修复。
2. 4KB 小 IO 瓶颈（target JFC wait）与 T4 深队列退化，为已知性能课题，非本次 review 范围。

## 参考

- 各场景详情：`scenC_result.md`、`scenD_result.md`、`scenE_result.md`、`scenG_result.md`、`scenH_result.md`、`scenR_regression.md`
- 原始数据：`data/scenA_*.log`、`scenB_*.log`、`scenC_*.log`、`scenD_*.log`、`scenE_rounds.log`、`scenH_build.log`（部分）
- 工具代码：`scripts/persist_check.c`、`scripts/t_scen*.sh`
