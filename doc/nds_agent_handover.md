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
| **197** | 141.61.41.197 | Initiator + 编译机 | 4× Ascend950PR（128GB HBM/卡）、CANN 系统级安装（**多版本并存** 8.5.0/9.0.1/9.1.0/9.0.T500，注意别混用）、10+ udmac 设备、openEuler 24.03 SP4 aarch64 内核 6.6.0-159。**多人共用，严守隔离守则** |
| **151** | 141.61.84.151 | Target | Tesla V100 + 12× NVMe 7.68T；系统盘 nvme9n1 绝对不能碰；md0 成员盘不能接管；空闲盘 nvme4n1（BDF 0000:a2:00.0）是当前测试盘。**已用 nds_v1 重建 target**（/home/l00955908/nds/spdk） |
| 245 | 141.61.84.245 | （已退出） | 曾当编译机与 gds UMDK 源，现不用；仅在需要 GPU 路线回归时可能回归 |

**连接方式**：用户会给你 SSH 通道（root + 端口 22），你通过用户中转操作，
**不要**自行扫描/尝试连接其他机器。

**外网情况（已更新）**：197 已可联网（git fetch/clone 实测可用，151 走代理
http://141.1.74.169:3128/ 也可达 GitHub）。代码更新首选 git pull；若某台
临时断网，备用流程：外部有网机器 pull 后 scp 上传（禁止 Windows 文本直传，
防 CRLF）。

## 3. 现有环境（已就绪，不要重建）

**197 上：**

| 路径 | 内容 |
|------|------|
| `/home/lx/spdk` | nds_v1 代码（已同步至 0f4bfaf）+ 已编译产物（urma_perf 含 -M npu/npu-staged） |
| `/home/lx/nds/UMDK_netlab` | gds 版 UMDK（含 is_gpu_seg/urma_register_seg_dmabuf 扩展，从 245 拷来） |

**运行 urma_perf 的固定姿势**（gds liburma 必须隔离加载，不能动系统库）：

```bash
LD_LIBRARY_PATH=/home/lx/nds/UMDK_netlab/lib \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M <cpu|npu|npu-staged|posix|peermem|dmabuf> -t 5
```

**151 上：** target 已用 nds_v1 重建（/home/l00955908/nds/spdk，
UMDK 在 /home/l00955908/nds/UMDK_netlab），批次 3R-1 实测 HELLO 通过；
旧的 /home/xxx/spdk（同事B 早期构建）已确认协议过时，退役。

## 4. 工作机制（对话文件 + 分批制）

- **对话文件**：仓库 `doc/nds_company_validation_task.md`（nds_v1 分支）
  - 第 9 节「内部 AI 回执区」：你把每步的**全部原始输出**追加到这里
  - 第 10 节「外部 AI 指令区」：外部开发 AI 的指令按编号排列（当前最新 #7）
- **分批制**：每批做完→回传→外部确认→解锁下一批。**禁止跳批/预支**。
- **回传方式**：197 连不了外网时，把追加后的对话文件全文交给用户带回。
- **commit 规范**：conventional commits，如 `docs(nds-task): 批次3R-1回执`。

## 5. 当前任务（进行到哪里了）

进度：批次 1（197 体检）✅ → 批次 2（gds 编译环境）✅ → 批次 3 ❌两个阻塞 →
批次 3R-1 ✅ **metadata 问题关闭**（151 重建 target 生效）→ 暴露两个新问题 →
**指令 #8「批次 4 诊断」待执行 ← 你从这里开始**

两个新问题及当前怀疑（详细命令在对话文件第 10 节「指令 2026-09-15 #8」）：

1. **LOC_ACCESS_ERR**（URMA pull 阶段被拒，-M cpu 也报）：重点怀疑 197 的
   发行版标准内核（6.6.0-159）URMA 驱动与 gds 定制内核行为有差异 →
   诊断：驱动 modinfo 对比 + 标准库对照实验 + dmesg 抓取
2. **aclrtSetDevice 507033**（顺序修复无效）：主嫌疑改为**多版本 CANN
   混用**（197 有 4 个版本）→ 诊断：CANN 调试日志 + 实际加载路径确认
   （新代码会打印 CANN runtime loaded: ...）

**执行指令 #8**（只读诊断为主，风险低），成功/失败都原样记录回传，
注明「批次 4 完毕」。

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
