# NDS 上机验证任务书（外部 ←→ 内部 AI 对话文件）

> 你好，内部 AI。本文件是**任务书 + 对话文件**，存放在 spdk-urma 仓库
> `nds_v1` 分支的 `doc/` 目录下。

## 对话协议（先读这个）

1. **读取任务**：`git pull origin nds_v1` 后读本文件，按第 0~6 节执行。
2. **回写结果**：把执行进展、全部命令输出和最终报告**追加到本文件末尾的
   「第 9 节 内部 AI 回执区」**（按日期分条目，报告全文贴入，不要另建文件、
   不要总结改写、不要删除/修改本文件已有内容）。
3. **回传**：`git add doc/nds_company_validation_task.md &&
   git commit -m "docs(nds-task): <本次做了什么>" && git push origin nds_v1`。
4. **往返**：外部 AI 会 pull 读取你的回执，把下一步指令追加到「第 10 节
   外部 AI 指令区」再 push；你随时 pull 查看新指令。
5. 卡住/异常时：不要自行发挥修复，原样记录现象后按第 2 步回传并注明
   "等待外部指令"。

- **原则**：所有命令输出必须原样保留（成功和报错都要）；不要修改任何代码；
  不要执行本文档列出之外的写操作/删除操作。

## 0. 背景一句话

我们在做 NDS（NPU HBM 经 URMA 直连远端 NVMe，仿照已有 GPU 版 GDS）。
外部开发已完成 SPDK 层代码（分支 `nds_v1`），本次上机目标：
**① 确认环境；② 编译通过；③ 跑通测试；④ 采集三个硬件验证项的数据。**

## 1. 机器角色识别（两台都要跑）

```bash
hostname; ip a | grep -E "inet " | head -5
npu-smi info
nvme list
```

判定规则：
- `npu-smi info` 有正常表格输出的机器 = **节点 1（Initiator，NPU 机）**
- `npu-smi` 不存在但 `nvme list` 有盘的机器 = **节点 2（Target，硬盘机）**
- 把两台的主机名/IP/角色记入报告。

## 2. 环境体检（原样记录全部输出）

**两台都跑：**

```bash
uname -r
cat /etc/os-release | head -3
lsmod | grep -E "udma|urma|ubus|ubcore"
which gcc make && gcc --version | head -1
```

**节点 1（NPU 机）跑——硬件验证三件套（最重要）：**

```bash
# ① 内核是否导出昇腾驱动的 pin 相关符号（决定 Phase 2 内核桥接怎么写）
cat /proc/kallsyms | grep -i davinci | head -50
cat /proc/kallsyms | grep -iE "hmm_|davinci.*pin|pin.*davinci" | head -30

# ② CANN 用户态库位置
find / -name "libascendcl.so" 2>/dev/null

# ③ CANN 是否有 dmabuf 导出能力（把 ② 找到的路径代入）
nm -D <②的路径>/libascendcl.so | grep -iE "dmabuf|handle|fd|export" | head -40
```

**节点 2（Target 机）跑——确认可用硬盘：**

```bash
nvme list
lsblk
lsblk -o NAME,MOUNTPOINT,TYPE,FSTYPE,SIZE
```

在报告里标注哪块 NVMe 盘是**空闲盘**（无 MOUNTPOINT、无 fstype、
非 LVM/RAID 成员），以及系统盘的 BDF（这台盘绝对不能动）。

## 3. 获取代码（节点 1）

二选一：

```bash
# A. 直接 clone（需 GitHub 凭证）
git clone -b nds_v1 https://github.com/yyyuanhao426-hash/spdk.git

# B. 如果外部已经 scp 拷贝了代码目录，直接 cd 进去
```

验证代码正确性：

```bash
cd spdk-urma
git log -1 --oneline    # 预期显示 ddbd44a feat(nds): SPDK 层 NPU Direct Storage 支持
ls examples/nvme/urma_perf/urma_perf_npu.c   # 预期存在
```

## 4. 定位 UMDK 并编译（节点 1）

```bash
# 找 UMDK（同事B环境里应已存在）
find / -name "urma_api.h" -not -path "*/spdk*" 2>/dev/null
```

`urma_api.h` 所在路径向上回溯到 UMDK 仓库根目录（其下应有
`src/urma/` 等目录），记为 `<UMDK>`。然后：

```bash
cd spdk-urma
./configure --with-urma=<UMDK> 2>&1 | tail -30
make -j$(nproc) 2>&1 | tail -50
ls -l build/examples/urma_perf
```

- 编译通过：报告里记录 configure/make 的最后 30~50 行 + urma_perf 文件路径
- 编译报错：**完整复制报错段落**（这是关键交付物之一），不要只给最后几行

## 5. 查找部署文档（决定能否跑测试）

同事B 的 GDS 测试有一份部署文档，但不在代码仓里。在两台机器上搜索：

```bash
find / -iname "*urma*deploy*" -o -iname "*spdk*deploy*" 2>/dev/null | head
find ~ -iname "*.md" 2>/dev/null | grep -iE "urma|spdk" | head
```

- **找到了**（如 `spdk_urma_deploment.md`）：阅读它，按文档在节点 2 启动
  nvmf_tgt（含 NVMe 盘接管、URMA transport 配置）。执行接管前**必须**先按
  文档核对待接管盘是空闲盘；启动后记录完整的启动命令和配置内容到报告。
- **没找到**：跳过第 6 节的运行测试，在报告里注明"部署文档缺失"，
  其余验证项照常交付。

## 6. 运行测试（节点 1；前提：第 5 节找到文档且 target 已就绪）

trid 模板（IP/子系统 NQN 按部署文档替换）：
`trtype:URMA adrfam:IPv4 traddr:<节点2 IP> trsvcid:4420 subnqn:<文档中的NQN>`

按顺序跑三条，每条完整记录输出：

```bash
# 6.1 回归：老功能确认未破坏（不需要 NPU）
sudo ./build/examples/urma_perf -r '<trid>' -M cpu -t 5

# 6.2 NPU staged 路线（本阶段主战场，预期通过预检并出带宽数据）
sudo ./build/examples/urma_perf -r '<trid>' -M npu-staged -t 5

# 6.3 NPU peermem 路线（预期失败——内核 NPU 桥接还没写，收集报错就是目的）
sudo ./build/examples/urma_perf -r '<trid>' -M npu -t 5
```

注意事项：
- 若 6.2 报 `Unable to load libascendcl.so`，用第 2 节 ② 找到的库目录设置
  环境变量后重跑：`export LD_LIBRARY_PATH=<CANN lib64 目录>:$LD_LIBRARY_PATH`
- 若 6.2 报 NPU 分配对齐相关错误，原样记录报错（这是页粒度验证项的输入）
- 6.3 预期在内存注册阶段失败（如 registration failure / -ENOTSUP / provider
  未注册类报错），**这个报错原文是 Phase 2 的关键输入**
- 每条测试结束把 stdout+stderr 全部记入报告

## 7. 输出报告要求

报告**全文**按下方模板写入「第 9 节 内部 AI 回执区」的最新条目里，
**所有"记录"处贴原始输出，不要总结改写**：

```markdown
# NDS 上机验证报告
日期：<date>  执行者：<内部AI标识>

## 1. 机器清单
| 角色 | 主机名 | IP | 关键硬件 |
|---|---|---|---|
| Initiator(NPU) | | | NPU 型号/数量（npu-smi 输出摘录）|
| Target | | | NVMe 盘型号/BDF |

## 2. 环境体检
### 2.1 两台公共
（uname/os-release/lsmod/gcc 原始输出）
### 2.2 节点1 硬件验证三件套
- 内核 davinci/hmm 符号：（kallsyms grep 原始输出）
  - 结论：有 / 无 可用于 pin HBM 的导出符号；列出最相关的符号名
- libascendcl.so 路径：
- nm -D 检查 dmabuf/handle/export：（原始输出）
  - 结论：有 / 无 dmabuf 导出类符号；列出符号名
### 2.3 节点2 硬盘
（nvme list / lsblk 原始输出；标注空闲盘 BDF 与系统盘 BDF）

## 3. 代码与编译
- git log -1 输出：
- configure 尾部输出：
- make 尾部输出：
- 结论：编译成功 / 失败（失败附完整报错）

## 4. 部署文档
- 是否找到：路径 / 未找到
- target 启动方式摘录（若找到）：

## 5. 测试结果
### 5.1 -M cpu（回归）
（完整输出）结论：通过 / 失败
### 5.2 -M npu-staged
（完整输出，含 mem_type=npu-staged 行、预检行、带宽/延迟行）
结论：通过 / 失败；staged_copy 耗时：
### 5.3 -M npu（预期失败）
（完整输出，重点：注册失败的报错原文）
结论：失败原因分析（初步即可）

## 6. 待解问题清单（执行中发现的任何异常）
```

## 8. 完成标志

报告生成后，外部团队需要的四样东西齐了：
1. davinci/hmm 内核符号有无可用的结论 + 原始输出
2. CANN dmabuf 能力结论 + nm 原始输出
3. 编译结果（成功或完整报错）
4. npu-staged 全链路结果 + npu 路线报错原文

齐了之后 commit + push，在回执区末尾注明「四项交付完毕，等待外部指令」。

## 9. 内部 AI 回执区

（内部 AI 在此下方按日期追加条目；每条以 `### 回执 YYYY-MM-DD HH:MM` 开头）

## 10. 外部 AI 指令区

（外部 AI 在此下方追加下一步指令；内部 AI 执行前先 pull）
