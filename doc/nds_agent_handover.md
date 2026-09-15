# NDS 测试 Agent 交接文档（新 Agent 必读）

> 你的前任（上一个测试 Agent）会话已结束，本文档帮你零基础接手。
> 读完后请先执行「第 5 节 当前任务」，不要做文档之外的事。

## 1. 项目目标

**NDS（NPU Direct Storage）**：让昇腾 NPU 的 HBM 显存绕过 host 内存、
经 URMA 总线直连远端 NVMe 盘读/写数据。这是对同事B已完成的 GDS
（GPU 版，Tesla V100，GPU HBM 直连 NVMe）的 NPU 复刻。

```
197（NPU 节点）                    151（node4）
┌──────────────────────┐          ┌──────────────────────┐
│ Initiator（发起端）   │          │ Target（目标端）      │
│ 4× Ascend950PR       │  URMA    │ NVMe 盘（空闲盘接管） │
│ + CANN + URMA 网卡   │ ◄──────► │ + URMA 网卡 + nvmf_tgt│
│ 跑 urma_perf 测试工具 │  +TCP    │ 不需要任何加速卡      │
└──────────────────────┘          └──────────────────────┘
```

测试工具是 SPDK 的 `urma_perf`（我们加了 `-M npu` / `-M npu-staged` 两个
NPU 内存路线），代码在同事B 的 GitHub 仓的 `nds_v1` 分支。

## 2. 机器与连接

| 节点 | IP | 角色 | 关键信息 |
|------|-----|------|---------|
| **197** | 141.61.41.197 | Initiator + 编译机 | 4× Ascend950PR（128GB HBM/卡）、CANN 系统级安装、10+ udmac 设备、openEuler 24.03 SP4 aarch64 内核 6.6.0-159。**多人共用，严守隔离守则** |
| **151** | 141.61.84.151 | Target | Tesla V100 + 12× NVMe 7.68T；系统盘 nvme9n1 绝对不能碰；md0 成员盘不能接管；空闲盘 nvme4n1（BDF 0000:a2:00.0）是当前测试盘 |
| 245 | 141.61.84.245 | （已退出） | 曾当编译机，现在不用；仅在需要 GPU 路线回归时可能回归 |

**连接方式**：用户会给你 SSH 通道（root + 端口 22），你通过用户中转操作，
**不要**自行扫描/尝试连接其他机器。

**外网限制（重要）**：197 连不了外网。代码更新统一走这个流程：

```
1. 外部有网机器（用户电脑或指定节点）：git pull origin nds_v1
2. scp/rsync 整个代码目录（或增量 diff）上传到 197 的 /home/lx/nds/spdk
3. 禁止用 Windows 工具直接文本传输（会引入 CRLF，见第 6 节教训）
```

## 3. 现有环境（已就绪，不要重建）

**197 上（用户目录 /home/lx/nds/）：**

| 路径 | 内容 |
|------|------|
| `/home/lx/nds/spdk` | nds_v1 代码 + 已编译产物（build/examples/urma_perf 含 -M npu/npu-staged） |
| `/home/lx/nds/UMDK_netlab` | gds 版 UMDK（含 is_gpu_seg/urma_register_seg_dmabuf 扩展，从 245 拷来） |

**运行 urma_perf 的固定姿势**（gds liburma 必须隔离加载，不能动系统库）：

```bash
LD_LIBRARY_PATH=/home/lx/nds/UMDK_netlab/lib \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M <cpu|npu|npu-staged|posix|peermem|dmabuf> -t 5
```

**151 上：** target 尚未用 nds_v1 重建（这正是当前任务，见第 5 节）；
旧的 /home/xxx/spdk（同事B 早期构建）已确认协议过时，退役。

## 4. 工作机制（对话文件 + 分批制）

- **对话文件**：仓库 `doc/nds_company_validation_task.md`（nds_v1 分支）
  - 第 9 节「内部 AI 回执区」：你把每步的**全部原始输出**追加到这里
  - 第 10 节「外部 AI 指令区」：外部开发 AI 的指令按编号排列（当前最新 #7）
- **分批制**：每批做完→回传→外部确认→解锁下一批。**禁止跳批/预支**。
- **回传方式**：197 连不了外网时，把追加后的对话文件全文交给用户带回。
- **commit 规范**：conventional commits，如 `docs(nds-task): 批次3R-1回执`。

## 5. 当前任务（进行到哪里了）

进度：批次 1（197 体检）✅ → 批次 2（gds 编译环境）✅ → 批次 3（首测）
❌ 暴露两个阻塞 → **指令 #7「批次 3R-1」待执行 ← 你从这里开始**

两个阻塞及外部已给的修复（都已提交，你需要拉最新代码）：

1. **URMA metadata 不兼容**（151 旧 target 与 nds_v1 协议不匹配）→
   修复 = 在 151 用 nds_v1 重建 target（7a）
2. **aclrtSetDevice 在 SPDK 进程内失败 507033**（DPDK EAL 干扰 CANN）→
   修复已改代码：npu 初始化提前到 spdk_env_init 之前（7c 验证）

**执行指令 #7**（详细命令在对话文件第 10 节「指令 2026-09-15 #7」）：

- 7a：151 上 clone nds_v1 + 拷 gds UMDK + 编译（`nice -n 10 make -j16`）
- 7b：151 用新树 `./target_nvme_takeover.sh -d nvme4n1` 起 target →
  197 重跑 `-M cpu` 回归（判定 metadata 问题是否关闭）
- 7c：197 跑 `-M npu-staged -t 1` 验证 507033 是否消失
  （若预检通过 = **NDS 全链路首测通过**，这是里程碑）
- 7d：恢复现场（151 还原盘/hugepages，197 还原 hugepages）

成功/失败都原样记录回传，注明「批次 3R-1 完毕」。

## 6. 已知问题与教训（前任踩过的坑）

1. **CRLF**：代码从 Windows 经 SFTP 上传会带 \r，configure 直接失败。
   一律 git clone 或 Linux 间 scp；绝不用 Windows 文本传输。
2. **编码**：对话文件是 UTF-8，不要用会转码的工具改写（前任外部 AI 踩过，
   整个文件中文变乱码后回滚重写）。
3. **liburma 版本**：197/151 系统自带的是标准版（无 is_gpu_seg 扩展），
   编译和运行都必须用 gds 版（/home/lx/nds/UMDK_netlab），
   运行时必须带 `LD_LIBRARY_PATH`。
4. **URMA 设备名**：197 有 10+ 设备（udmac0d1e2e6 等），先 `find /sys -name '*udmac*'`
   记录，连接失败时再用 SPDK_URMA_DEV_NAME 指定重试。
5. **hugepages**：SPDK 需要大页。跑前 `grep -i huge /proc/meminfo` 记录，
   跑后确认恢复（守则第 4 条）。
6. **ACL 初始化顺序**：npu 初始化必须在 spdk_env_init 之前（代码已修，
   不要"顺手"改回去）。

## 7. 隔离守则（摘要，全文见对话文件 0.5 节）

197 是多人共用机：产物只进自家 home；禁止全盘 find（限定路径+timeout）；
编译 `nice -n 10 make -j16`；hugepages 跑前记录跑后恢复；绑核从少（默认
-T 1）；URMA 设备先确认无他人使用；严禁装卸内核模块；覆写只允许对已确认
的空闲盘；影响他人立即停止并回传。

## 8. 关键参考

- 设计文档：`doc/nds_design.md`（三层架构、Phase 2 方向）
- 对话文件：`doc/nds_company_validation_task.md`（全部历史回执与指令）
- Phase 2 方向（了解即可，暂不做）：davinci pin 符号存在但未导出（不能直调），
  主攻 CANN 导出（aclrtMemExportToShareableHandle）→ dma-buf 注册路线
