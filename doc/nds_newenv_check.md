# NDS 新环境体检任务书（新机器 × 2，只读体检）

> 你好，测试 Agent。NDS 项目的上一套测试机器被占用，现在换到**新的两台机器**。
> 本批任务**只做环境体检，全部是只读命令**：不装软件、不改配置、不跑测试、
> 不编译。所有命令输出**原样保留**（成功和报错都要），最后按第 6 节模板
> 输出报告 `nds_newenv_report.md` 交回。

## 0. 背景（一段话）

NDS = 让昇腾 NPU 的 HBM 显存经 URMA 总线直连远端 NVMe 盘（已验证的
GPU 版 GDS 的 NPU 复刻）。部署需要两台机器：**一台带 NPU + CANN +
URMA 网卡（Initiator）**，**一台带空闲 NVMe 盘 + URMA 网卡（Target）**。
你的任务：确认这两台新机器是否满足条件，并采集后续开发需要的环境数据。

## 1. 机器识别（两台都跑）

```bash
hostname; ip a | grep "inet " | grep -v 127.0.0.1
npu-smi info
nvme list
uname -m
```

判定：`npu-smi info` 有表格输出的 = **节点 A（NPU 机 / Initiator 候选）**；
`nvme list` 有盘的 = **节点 B（Target 候选）**。两台都可能兼有，如实记录。

## 2. 两台公共项（原样记录）

```bash
uname -r
cat /etc/os-release | head -3
lsmod | grep -E "udma|urma|ubus|ubcore|ubase|ummu"     # URMA 驱动栈是否加载
for m in ubcore uburma udma ummu ummu_core ubus ubase; do \
  echo "== $m =="; modinfo $m 2>/dev/null | grep -E "^(filename|version|srcversion)"; done
which gcc make git; gcc --version | head -1
curl -sI --max-time 10 https://github.com | head -3     # 外网连通性
df -h /home | tail -1                                   # home 剩余空间（编译要 ~10G）
```

> 驱动版本对比是重点：NDS 之前在一台"标准内核"机器上遇到过数据面
> 访问被拒，在一台"gdr 定制内核"机器上验证正常。请记录两台的
> 内核版本与驱动 srcversion，供外部判断与历史结论的关系。

## 3. 节点 A（NPU 机）专查

```bash
npu-smi info                                # NPU 型号/数量/健康
find /usr/local/Ascend -maxdepth 3 -name "libascendcl.so" 2>/dev/null
ls /usr/local/Ascend 2>/dev/null            # CANN 版本目录
# CANN dmabuf/导出能力（路径按上条结果代入）
nm -D <libascendcl.so 路径> | grep -iE "Export|Shareable|MallocPhysical|ImportFrom" | head -20
# 内核 davinci pin 符号（历史结论：存在但未导出，确认新机器是否同样）
cat /proc/kallsyms | grep -i davinci | head -20
cat /proc/kallsyms | grep -E "vdavinci|davinci.*pin" | head -20
```

## 4. 节点 B（Target 机）专查

```bash
nvme list
lsblk -o NAME,MOUNTPOINT,TYPE,FSTYPE,SIZE
```

- 标出**空闲盘**（无 MOUNTPOINT、无 fstype、非 LVM/md 成员）及其 BDF
  （`nvme list` 无 BDF 则用 `ls -l /dev/disk/by-id/` 或 `nvme list -o device` 辅助）
- 标出**系统盘**（有 / 或 /boot 的）——绝对不能碰
- 标出 md/LVM 成员盘——不能接管

## 5. 双机互联与共享情况

```bash
# 两台互 ping（在各自机器上跑对方的 IP）
ping -c 3 <对方 IP>
```

另外向管理员/用户确认并记录：
1. 两台是否**多人共用**（决定隔离级别）
2. 两台之间除了 TCP，URMA 网络是否规划互通（同 UB 域）
3. 是否有 root 权限、home 目录可用空间

## 6. 报告模板（输出 `nds_newenv_report.md`）

```markdown
# NDS 新环境体检报告
日期：  执行者：

## 1. 机器清单
| 节点 | 主机名 | IP | 角色 | NPU/盘 | 内核 | 是否共用 |
|---|---|---|---|---|---|---|

## 2. 公共项
（uname/os-release/lsmod/modinfo 版本表/gcc/外网/磁盘空间 原始输出）
- 两台 URMA 驱动 srcversion 是否一致：

## 3. 节点 A（NPU 机）
- npu-smi 原始输出：
- CANN 版本与 libascendcl.so 路径：
- nm -D 导出接口检查（原始输出）：
- 内核 davinci 符号（原始输出）：
- 结论：满足 Initiator 条件？缺什么？

## 4. 节点 B（Target 机）
- nvme list / lsblk 原始输出：
- 空闲盘候选（盘名+BDF）：
- 系统盘（绝对禁碰）：
- 结论：满足 Target 条件？缺什么？

## 5. 互联
- 双机 ping 结果：
- 结论：网络互通？

## 6. 待解问题 / 异常清单
```

## 7. 完成标志

报告生成后交回（或按用户指示追加进主对话文件
`doc/nds_company_validation_task.md` 的回执区）。本批**不做**任何部署、
编译、测试——后续批次由外部 AI 根据报告另行下发指令。
