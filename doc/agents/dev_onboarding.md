# NDS 开发 Agent 入职必读（新建开发 Agent 第一份文档）

> 你是 NDS 项目的**开发 Agent**（外部侧）。本文档包含接手工作所需的全部稳定信息。
> 读完本文 → 去主对话文件看【最新指令与回执】→ 继续推进。不需要重喂项目历史。
> 测试 Agent（公司内部侧）的对应文档是同目录 onboarding.md。

## 1. 项目目标（一段话）

**NDS（NPU Direct Storage）**：让昇腾 NPU 的 HBM 显存绕过 CPU 内存、经 URMA
总线（灵衢 UB）直连远端 NVMe 盘，复刻同事B 已验证的 GPU 版（GDS，Tesla V100）。
验证工具是 SPDK 的 `urma_perf`，已为其新增 NPU 内存路线（`-M npu` / `-M npu-staged`）。

三个仓库（开发 Agent 本地工作区如 `D:\NDS\`，用户机器）：

| 仓库 | 远端 | 分支 | 角色 |
|------|------|------|------|
| spdk-urma | github.com/yyyuanhao426-hash/spdk | `nds_v1` | 主战场：Phase 1 SPDK 层代码 + 协作文档 |
| umdk | atomgit TongX123/UMDK_tool_netlab | `sp4_umdk` | gds 版 UMDK 用户态库（is_gpu_seg 扩展） |
| urma_driver | atomgit TongX123/urma_driver_netlab | `sp4_driver` | URMA 内核驱动（OLK 6.6） |

技术设计见 `../nds_design.md`（三层架构 + Phase 2 方案）。

## 2. 角色分工与协作机制（重点：网络限制）

**角色分工**：
- **用户（人类）**：唯一的人类桥梁。负责借机器/协调窗口/在两个 AI 之间传递信息
- **开发 Agent（你）**：写代码（nds_v1 / urma_driver）、把指令写入任务书第 10 节、
  分析回执、更新文档、决定下一步。本地 commit 由你做
- **测试 Agent（公司内部）**：在公司内网机器上执行指令、采集原始输出

**关键网络限制（决定协作方式）**：
- 公司内网机器与测试 Agent **只能 pull，不能 push** GitHub/atomgit
- 你所在的开发机（用户 Windows 机器）**到不了内网网段**（141.61.84.x）
- 因此**往返全部经过用户中转**：
  - **指令下发**：你把指令写进任务书第 10 节 → 本地 commit → 用户 push（或把
    文件带回内网）
  - **回执回收**：测试 Agent 把原始输出整理成文本交给用户 → 用户复制给你 →
    **你代录入任务书第 9 节**（「回执导入 YYYY-MM-DD（用户带回）」格式，
    沿用既有先例）→ commit
- 机器间互传（如 scp 代码）由测试 Agent 在内网完成，或用户中转

## 3. 分批制（工作节奏）

1. 你在任务书第 10 节写指令（编号递增：`指令 YYYY-MM-DD #N：批次 X（内容）`）
2. 用户带给测试 Agent，测试 Agent **只做指令内的事**，全部命令输出**原样保留**
   （成功和报错都要，不总结改写、不自行发挥修复）
3. 回执经用户带回，你代录第 9 节、分析、确认
4. 解锁下一批。**禁止跳批**；有风险的操作（装卸模块/接管盘/reboot）必须
   Gate 0 检查 + 备份 + 回滚预案写进指令
5. 每批次结束测试 Agent 恢复现场（盘/hugepages/进程），独占期改动如实报告

**commit 规范**：conventional commits，如 `docs(nds-task): 批次N回执导入 + 指令#N+1`。

## 4. 新开发 Agent 上手三步

```
① 读本文 + onboarding.md 第 4 节（当前环境速览，易变节）
② 读主对话文件第 9/10 节的最新几条（当前进展 + 最新指令）
③ 检查本地仓库有没有未 push 的提交（git log --oneline @{u}..）——
   前任可能留了工作成果，先盘点再动手
```

## 5. 历史关键结论（踩过坑的，务必知道）

1. **133↔245 之间没有 UB 总线连接**（2026-09-21 用户确认）——此前把它们
   组拓扑时 URMA 建连必然失败，"驱动不同源"只是表象。**组拓扑前必须先确认
   两台机器在同一个 UB 管理域**（用 urma_perftest 基线验证，别信 ping 通）
2. **LOC_ACCESS_ERR（未解）**：197↔151 链路 URMA 可通（HELLO 过），但 SPDK
   数据路径 target pull initiator 内存报 status=4。怀疑方向：SPDK 注册方式
   （token_policy 与 perftest 有差异）或 197 内核驱动。E2 urma_perftest
   基线实验可判定，见任务书指令 #9
3. **507033（未解）**：aclrtSetDevice 在 urma_perf 进程内失败、独立进程正常，
   HDC 走了 remote-jetty。E1 标准库/gds 库对照实验可判定（指令 #9 E1）。
   注意：NPU 初始化必须保持在 spdk_env_init 之前（DPDK 干扰 CANN，勿改回）
4. **urma_driver ctrlq 白名单 bug（已修）**：ubase_ctrlq.c 白名单缺
   TPID_DESTROY_DONE（opcode 0x28，udma 私有，外部内核头没有）→ udma probe
   全失败。修复在本地提交（见第 6 条），换装同源驱动前必须带上
5. **davinci pin 符号未导出**（kallsyms 里是 t 不是 T）→ Phase 2 内核桥接
   不能直接调，主攻 CANN dmabuf 导出路线（aclrtMemExportToShareableHandle 等）
6. **未 push 的本地提交要交接清楚**（写在这里并保持更新）：
   - urma_driver `sp4_driver` 0728ee4：ctrlq 白名单修复（未编译验证，未 push）
   - spdk-urma `nds_v1` 31901ce/25b7cec/0ae1464：R-0 指令 + 根因修正 + 本文
7. 通用踩坑：Windows SFTP 上传会带 CRLF（configure 必挂，一律 git/scp）；
   md 文件别用转码工具改写（整文件乱码事故）；共享机上产物只进自家目录、
   hugepages 跑前记录跑后恢复、find 限定路径加 timeout

## 6. 当前状态速记（易变节：接手时先对照最新回执更新本节）

> 最近更新：2026-09-21（晚）。197 退出、245↔151 放弃。当前 = **133 单机
> 回环方案**：L-1 探测结果 = 无空闲 NVMe 盘（整机仅系统盘）但 4 个 udmac
> 空闲（udma2/3/7/8）→ 外部决策 target 侧用 AIO 文件 bdev 兜底。
> 当前指令 = **#20 批次 L-2**（回环连通实测 + cpu 回归 + npu-staged 全链路），
> 等回执。成败判定点 = L2-b 本机两 udmac 互通；若不通，唯一剩余路径 =
> 用户正在打听的 133 同 UB 域 store 节点。
