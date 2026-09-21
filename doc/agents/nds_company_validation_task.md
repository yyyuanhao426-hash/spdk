# NDS 上机验证任务书（外部 ←→ 内部 AI 对话文件）

> 你好，内部 AI。本文件是**任务书 + 对话文件**，存放在 spdk-urma 仓库
> `nds_v1` 分支的 `doc/agents/` 目录下（协作文档中心，见 doc/agents/README.md；
> 新建 Agent 请先读同目录 onboarding.md）。

## 对话协议（先读这个）

1. **读取任务**：`git pull origin nds_v1` 后读本文件，按第 0~6 节执行。
2. **回写结果**：把执行进展、全部命令输出和最终报告**追加到本文件末尾的
   「第 9 节 内部 AI 回执区」**（按日期分条目，报告全文贴入，不要另建文件、
   不要总结改写、不要删除/修改本文件已有内容）。
3. **回传**：`git add doc/agents/nds_company_validation_task.md &&
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

## 0.5 共享节点隔离守则（197 为多人共用，必须遵守）

197 节点有很多用户共用，所有操作以"不打扰他人"为前提：

1. **产物隔离**：代码、编译产物、临时文件全部放自己 home 目录
   （如 /home/l00955908/nds/），禁止写入 /home/xxx、/home/yin 等他人目录
   （可读不可写），禁止写 /tmp 之外的系统目录。
2. **搜索限域**：禁止 `find /` 全盘搜索；限定路径（/usr/local/Ascend、
   /usr/lib64、/opt、$HOME）并加超时，如
   `timeout 60 find <路径...> -name <pattern> 2>/dev/null`。
3. **编译限核**：`make -j16`（不要 -j$(nproc)），可加 nice 降优先级：
   `nice -n 10 make -j16`；编译尽量错峰（避开整机高负载时段）。
4. **SPDK 大页内存**：urma_perf 初始化会消耗 hugepages。跑前先记录现状
   （`grep -i huge /proc/meminfo`），只做短时测试（-t 5），测完确认大页
   释放；若需要调大 hugepages，测后必须恢复原值并记录。
5. **绑核限定**：urma_perf 用 `-m` 显式指定少量核（如 `-m 0x3` 只用头两个核），
   不要让 SPDK 占满所有核。
6. **URMA 设备共享**：URMA 网卡/设备是整机共享的，跑测试前确认无他人在用
   （如问管理员/看是否有其他 URMA 进程），测试窗口尽量短。
7. **不改系统配置**：不安装/卸载系统包、不改内核参数（除非第 4 条的
   hugepages 且须恢复）；**严禁**在共享机上 insmod/rmmod 任何内核模块
   （包括将来 Phase 2 的 .ko，届时单独协调维护窗口）。
8. **破坏性写保护**：urma_perf 会覆写 target 盘数据——只允许对已确认的
   空闲盘/专用测试盘进行，且跑前记录、跑后注明。

违反以上任一条导致影响他人时，立即停止操作、原样记录、回传等待指令。

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
# 找 UMDK（同事B环境里应已存在；限定路径，禁止全盘 find）
timeout 60 find /usr/local /usr/lib64 /opt /home -name "urma_api.h" -not -path "*/spdk*" 2>/dev/null
```

`urma_api.h` 所在路径向上回溯到 UMDK 仓库根目录（其下应有
`src/urma/` 等目录），记为 `<UMDK>`。然后：

```bash
cd spdk-urma
./configure --with-urma=<UMDK> 2>&1 | tail -30
# 共享机限核编译（守则第 3 条：禁用 -j$(nproc)）
nice -n 10 make -j16 2>&1 | tail -50
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

### 回执导入 2026-09-15（外部 AI 代录）

内部 AI 完整回执由用户以本地文件带回（外部副本 d:\NDS\nds_company_validation_task.md，
含全部原始输出）。以下仅录关键事实，全文以用户副本为准：

- 机器：node4=141.61.84.151（Tesla V100，12 块 NVMe，空闲盘 nvme4n1/nvme7n1，
  系统盘 nvme9n1）；node1=141.61.84.245（RTX 4090 D，11 块 NVMe，
  空闲盘 nvme4~11n1，系统盘 nvme3n1）。**两台均无 NPU/CANN/davinci**。
- 内核 6.6.0（gdr_w00921547+ / netlab_pcie_ub_compat+），openEuler 24.03 LTS-SP4，
  **AArch64**，URMA 驱动栈已加载（udma/ubcore/uburma/udma_nv_p2p_bridge 等）。
- 编译失败：`CONFIG.sh: line 7: $'\r': command not found` → `Configuration failed`，
  EXIT=127（SFTP 上传致 CRLF 行尾）；mk/config.mk 未生成。
- 代码位置 /home/l00955908/nds/spdk（SFTP 上传后重新 git init，历史非原始 ddbd44a）。
- UMDK：node1 完整源码树 /home/yin/gdr/UMDK_netlab（configure 可识别）；
  node4 系统库 /usr/lib64/liburma.so + 头文件 /home/xxx/urma_include。
- 部署文档未找到；但发现同事B 的 run_urma_perf_v6.sh（trid：traddr=141.61.84.151,
  trsvcid=4420, subnqn=nqn.2026-01.io.spdk:urma-gpu-test；DEV_NAME=udmac1d1e2；
  LD_PATH=/home/tong/umdk_gpu_isolated/lib）与 target_nvme_takeover.sh 一键脚本
  （两台均存在）。
- 四项交付物：NPU 相关三项无法交付（无 NPU 机器）；编译受阻待修。

## 10. 外部 AI 指令区

### 指令 2026-09-21 #17：批次 R-0（197 可用性侦察，只读）

**背景（重要，已确认的根因）**：用户确认 **133 与 245 之间没有 UB 总线
连接**——133↔245 URMA 建连失败的真正根因，驱动换装方案作废，245 退出。
计划回归 197（Initiator，950PR）+ 151（Target）拓扑（该链路 URMA 已验证
可通）。**本批只做 197 可用性侦察，全部只读**；确认可用后外部再下发
回归执行批（R-1）。

**A. 197 连通性（用既有登录方式）**：
```bash
ping -c 3 141.61.84.197
# 能登录则继续；登录方式照旧（ssh/sshpass）
```

**B. 197 是否被占用**：
```bash
who
ps aux | grep -E "nvmf_tgt|urma_perf|vllm" | grep -v grep | head
```

**C. 我方环境残留**：
```bash
ls -ld /home/lx/spdk /home/lx/nds/UMDK_netlab 2>/dev/null
cd /home/lx/spdk && git log --oneline -1 && git status -s | head -5
```

**D. 197 基本状态**：
```bash
npu-smi info | head -20
uname -r
grep -i huge /proc/meminfo
```

**E. 151 顺带确认（用户已表示可随时借用）**：
```bash
ping -c 3 141.61.84.151
# 能登录则：ls -ld /home/l00955908/nds/spdk && nvme list | head
```

**判定与回传**：197 空闲 + 代码树在 → 回传「R-0 通过，可下发 R-1」；
被占用或代码树丢失 → 原样记录回传（都有办法处理，不要现场发挥）。
全部输出追加第 9 节，注明「批次 R-0 完毕」。

### 指令 2026-09-15 #9：批次 5（两个隔离实验）

前置：批次 4 已回执。**外部已提交代码改造**：urma_register_seg_dmabuf 改为
运行时 dlsym 解析（RTLD_DEFAULT），同一份 urma_perf 二进制现在可以用
标准 liburma 跑 host-only 路线（cpu/posix/npu-staged）——批次 4 被阻断的
对照实验已解锁。

```bash
# 197：先拉代码并增量重编（含诊断打印 + 本次改造）
cd /home/lx/spdk && git pull origin nds_v1
nice -n 10 make -j16 2>&1 | tail -10
```

**E1. 507033 触发源隔离（197，核心实验；需 151 target 已按 7b 启动）**

背景：批次 4 证实 HDC 在 urma_perf 进程内走了 remote-jetty 路径且全败。
主嫌疑：进程加载的 gds liburma（及其插件库）让 HDC 选择了 UB/远程路径。
现在可用两种 liburma 分别跑 npu-staged 对比：

```bash
# E1-a. 标准 liburma（LD_LIBRARY_PATH 只含 CANN，不含 UMDK）
LD_LIBRARY_PATH=/usr/local/Ascend/cann-9.0.T500/aarch64-linux/lib64 \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M npu-staged -t 5

# E1-b. gds liburma（对照组，原配置重跑确认 507033 仍复现）
LD_LIBRARY_PATH=/home/lx/nds/UMDK_netlab/lib:/usr/local/Ascend/cann-9.0.T500/aarch64-linux/lib64 \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M npu-staged -t 5
```

判定：
- **结果 A（E1-a 507033 消失且预检通过）= NDS 全链路首测通过 🎯**，且锁定
  根因 = gds liburma 在进程中触发 HDC remote-jetty 路径 → 外部再定根治方案
- 结果 B（E1-a 仍 507033）→ 排除 gds liburma，下一步查 DPDK/EAL 与 HDC 交互
- 结果 C（E1-a URMA 连接失败）→ 记录标准库差异报错

**E2. LOC_ACCESS_ERR 基线（绕开 SPDK，直接测 197 内核 URMA 路径）**

E2-a. 找或构建 urma_perftest（UMDK 自带工具，纯 URMA 层双向读写，
完全绕开 SPDK，可判定 197 内核驱动是否有问题）：
```bash
find /home/lx/nds/UMDK_netlab /home/l00955908/nds -name "urma_perftest" -type f 2>/dev/null
# 若没有：UMDK 树 src/urma/tools/urma_perftest 有源码，根目录有 CMakeLists.txt；
# 或到 245 上 find /home -name "urma_perftest"（同事A 编译过 UMDK，可能有现成二进制，
# 同为 aarch64 可直接拷）
```
E2-b. 双机基线：151 起 server、197 起 client，纯 host 内存最简模式
（命令按 urma_perftest -h，不要加 GPU 参数）：
- **perftest 通过** → 197 内核 URMA 路径正常，LOC_ACCESS_ERR 出在
  SPDK/gds-liburma 的注册方式（外部下一步对照 perftest 的 token_policy/
  access 配置——本地源码已发现两者有差异，perftest 用
  token_id_valid + token_policy + ATOMIC，SPDK 用 TOKEN_NONE + 无 ATOMIC）
- **perftest 也报 access error** → 197 内核驱动问题实锤 → 后续方案：
  用 urma_driver 源码为 197 内核编译驱动模块（共享机装卸需协调维护窗口）

E2-c. 顺手补批次 4 没做成的对照（现在不会 undefined symbol 了）：
```bash
# 标准 liburma 下的 -M cpu（LOC_ACCESS_ERR 是否与 gds liburma 相关）
LD_LIBRARY_PATH=/usr/local/Ascend/cann-9.0.T500/aarch64-linux/lib64 \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M cpu -t 5
```

**回传**：全部输出追加第 9 节，注明「批次 5 完毕」。

### 回执导入 2026-09-20 #7（批次 A'' 结果，用户带回）

- 清场 ✅：kill p50065573 残留 target（PID 627047，已授权），hugepages 释放
- 245 起 target：**gds 版 liburma 在 245 上 urma_init 失败**（它是同事A 为
  gdr 定制内核编译的，与两机现内核均不匹配）→ 改用系统版 /usr/lib64 成功
  （target 侧本不需 gds 扩展）；RPC 全部配置完成
- 133 首测 ❌：**URMA 建连握手在驱动层失败**，尝试矩阵（gds/系统 ×
  单端口/bonding）全败；245 侧 dmesg：单端口 `ubmad_post_send: get primary
  eid failed ret=-2`；bonding `ubagg_import_jetty: failed to exchange udata`
- 拓扑不对称：133 = 10 udma 无 bonding；245 = 6 udmac + bonding_dev_0
- 结论：**阻塞升级为基础设施问题**——两机 URMA 内核驱动不是同一套、
  UB 管理面未为 133↔245 配置互通；gds liburma 也需针对实际环境重建。
  代码侧已就绪，SPDK 层无法绕过驱动层问题，转内部协调
- 现场已还原 ✅（改动清单完整）

### 回执导入 2026-09-20 #8（批次 C-2 结果，用户带回）

- 结论：**换装失败 → 回滚成功**，现场已还原，245 归还给公共池
- C2-a ✅：停止残留 target（p50065573 的 nvmf_tgt PID 2127783，二次出现）
- C2-b 方法一 ❌：ubase/ubus/ubfi 被非 URMA 组件（cis/ub_fwctl/unic）占用
  无法卸载，转方法二
- C2-c 方法二：替换 19 个 .ko + reboot 生效（中间一次 grub 默认项切到
  标准内核的意外已修正）；16 个核心模块加载新 srcversion（c12ec44）
- C2-d ❌ **换装后 udma 设备 probe 全部失败**：/dev/uburma 缺失；dmesg
  `ubase register ctrlq event failed opcode=40 ret=-2` → udma probe -22；
  **根因（源码级）**：atomgit c12ec44 基线中 udma_eq.c 注册了
  UDMA_CMD_CTRLQ_TPID_DESTROY_DONE（服务类型 TP_ACL），但 ubase 的
  ctrlq 白名单 ubase_ctrlq_wlist_udma 缺该 opcode → 注册返回 -ENOENT →
  probe 失败。**外部已在本地源码验证确认该不一致存在**（udma_eq.c:991 vs
  ubase_ctrlq.c:22），修复方案留待下次（下次再说）
- C2-f 回滚 ✅：备份 .ko 恢复 + depmod + reboot，旧 srcversion 全部恢复，
  /dev/uburma 6 设备齐全，grub 默认回标准内核（与占用前一致）；
  残留：udma_nv_p2p_bridge 回滚后 insmod 失败（依赖 nvidia 栈未加载，
  非核心，已记录）
- 现场改动清单完整（独占期如实报告）

### 指令 2026-09-20 #16：245 归还核对清单（最后一步）

245 使用结束，归还公共池前做最终核对（只读为主）：

```bash
ps aux | grep -E "nvmf_tgt|urma_perf|spdk" | grep -v grep   # 无残留进程
lsblk -o NAME,MOUNTPOINT,TYPE,FSTYPE,SIZE                   # 12 盘齐全且在 nvme 驱动
grubby --default-kernel                                     # 应为标准内核（占用前默认值）
grep -i huge /proc/meminfo                                  # 记录当前值
modinfo ubcore uburma udma | grep srcversion                # 应为旧值（21A93C/6AA3EA/8E42E2）
ls /dev/uburma/ 2>/dev/null                                 # URMA 设备节点齐全
```
- 全部符合 → 在 batchA_receipt.md 末尾追加「245 归还核对通过」段（含上述
  输出），245 即可交还；home 目录下我方文件（/home/lx、/home/l00955908/nds）
  **保留不删**（下次环境可能复用），是否清理听从机器管理方
- 不符合项 → 原样记录回传，不要现场发挥修复

### 指令 2026-09-20 #15：批次 C-2（245 驱动换装 + 互通验证）

**外部决策（对 C-1 三个风险点）**：① GPU P2P 启用保留（245 旧 bridge 本就
启用，wire 兼容不要求 srcversion 一致，且为 Phase 2 保留基础）；
② ummu/ummu_core 内核自带无法对齐，接受残留风险（历史失败点在
ubcore/ubus 层，本次换装已覆盖）；③ ubase/ubus 连带依赖风险用双方法+
回滚应对。前提：独占窗口内、C1-d 备份完好、所有用户态 SPDK/URMA 应用已停。

**C2-a. 停用户态**：确认 245 无 urma_perf/nvmf_tgt/其他 URMA 应用（有则停）。

**C2-b. 方法一（在线换装，优先）**：
```bash
# 按批次 C1-e 的反向顺序卸载（建议用 modprobe -r 让内核自动解析依赖，
# 从叶子 udma_nv_p2p_bridge 开始）。任一模块报 Module is in use：
#   记录占用者 → 若可停则停后重试 → 不可停（如 mami/fabric 类服务）→ 转方法二
modprobe -r udma_nv_p2p_bridge    # 从叶子开始，逐个按 C1-e 反向顺序
# ... 全部卸载后，替换文件：
# 把新构建 .ko 拷到各模块原 filename 位置（备份已在 /home/lx/driver_backup）
# 注意保持原文件名/路径/压缩格式不变
depmod -A
# 正向加载（按 C1-e 顺序）：ubase → ubcore → ubfi → ubus → hisi_ubus →
# cdma/obmm → udma → uburma/ubagg/ipourma → sentry 系列 → udma_nv_p2p_bridge
modprobe ubase    # 逐个按顺序，或 insmod <路径>
```

**C2-c. 方法二（方法一失败时的回退式换装）**：
```bash
# 直接替换 .ko 文件（同 C2-b 的替换步骤）→ reboot
# 重启后检查：lsmod 是否自动加载（先查 /etc/modules-load.d/ 与
# /etc/modprobe.d/ 判断是否开机自动加载）；未加载则手动按顺序加载
```

**C2-d. 换装后验证（245 本机）**：
```bash
lsmod | grep -iE "ub|urma|udma"        # 全部新模块加载
dmesg | tail -50                        # 无驱动报错
ls /dev | grep -i uburma                # 设备节点存在
modinfo ubcore | grep srcversion        # 应为 53C2130...（c12ec44）
# 起 target 验证（依次尝试两种 liburma）：
LD_LIBRARY_PATH=/usr/lib64 \
  ./target_nvme_takeover.sh -d nvme0n1 -L /usr/lib64 -i 141.61.84.245 -y
# 若 urma_init 失败，换 gds liburma 再试：
LD_LIBRARY_PATH=/home/lx/UMDK_netlab/lib:/usr/local/Ascend/cann-9.1.0/aarch64-linux/lib64 \
  ./target_nvme_takeover.sh -d nvme0n1 -L /home/lx/UMDK_netlab/lib -i 141.61.84.245 -y
```

**C2-e. 互通验证（133，target 已在 245）**：
```bash
# 133 跑 cpu 回归（标准 liburma）——判定驱动同源后互通是否成立：
LD_LIBRARY_PATH=/home/lx/nds/spdk/build/lib:/usr/local/Ascend/cann-9.1.0/aarch64-linux/lib64 \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.245 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M cpu -t 5
# 通过 → 同命令换 -M npu-staged 跑全链路首测（NPU 卡选 OK 且空闲者，-g 显式）
```

**判定与分支**：
- 互通通过 → **保持新驱动**（不回滚），继续批次 A'' 剩余项；这是里程碑
- 互通仍失败 → 回滚（rmmod 新 → 恢复 /home/lx/driver_backup 备份 .ko →
  depmod -A → 重载原模块），记录后转方案①（管理面）/③（换机器）

**C2-f. 回传**：全部输出（含方法一/二的执行轨迹、互换后 srcversion、
互通结果）追加第 9 节，注明「批次 C-2 完毕」。

### 指令 2026-09-20 #14：批次 C-1（245 同源驱动：构建 + 备份，不换装）

**用户已选方案②**：在 245 用 urma_driver 源码构建与 133 同源的 URMA 驱动
（源码仓 openEuler SP4 同步 + GPU 补丁，133 的驱动即同一基线编译），换装后
两机驱动同源 → 解决互通问题。**本批只做构建与备份，不卸载/替换任何
运行中的模块**；换装（C-2）待外部确认后另批执行。

**Gate 0（先做，任一不满足则中止回传）：**
```bash
ls -l /lib/modules/$(uname -r)/build     # 内核构建目录必须存在
which gcc make && rpm -q kernel-devel 2>/dev/null
```

**C1-a. 获取 urma_driver 源码：**
```bash
# 优先：直接 clone（AtomGit，默认分支 sp4_driver）
git clone https://atomgit.com/TongX123/urma_driver_netlab.git /home/lx/urma_driver
cd /home/lx/urma_driver && git log --oneline -1   # 记录 HEAD
# （无法访问 AtomGit 时：外部本地有副本，用户可 scp 过来）
```

**C1-b. 构建（245 无 NVIDIA，GPU P2P 会自动禁用，Target 不需要它）：**
```bash
cd /home/lx/urma_driver
make 2>&1 | tail -40
# 记录产出：find . -name "*.ko" | sort
```

**C1-c. 模块集对比（关键）：**
```bash
# 当前运行中的 URMA 相关模块及其磁盘位置
lsmod | grep -iE "ub|urma|ummu|udma|cdma|obmm|sentry|ubfi|ubagg|ipourma|hisi" \
  | awk '{print $1}' | while read m; do \
    echo "$m => $(modinfo $m 2>/dev/null | grep '^filename')"; done
# 构建产出的模块列表
find /home/lx/urma_driver -name "*.ko" | sort
```
- 逐个比对：运行中的模块是否都能在构建产出中找到对应 .ko？
- **若有运行中模块不在构建产出里（混合版本风险）→ 停止，原样回传等待决策**

**C1-d. 备份（回退保障）：**
```bash
mkdir -p /home/lx/driver_backup
lsmod > /home/lx/driver_backup/lsmod_before.txt
# 把当前加载的每个 URMA 模块的磁盘文件原样拷贝到备份目录（保留目录结构）
# 并记录 modinfo filename 与卸载顺序建议（按 lsmod 依赖反向）
```

**C1-e. 换装演练清单（只写在报告里，不执行）：**
按依赖反向的 rmmod 顺序、正向的 insmod 顺序、depmod -A 命令、
验证命令（lsmod/dmesg/urma 设备列表）、回退步骤（rmmod 新 + 恢复备份 .ko +
depmod + 重新加载）。

**回传**：全部输出 + 模块对比表 + 换装演练清单，注明「批次 C-1 完毕」。
外部确认后才下发 C-2（换装）指令。

### 指令 2026-09-20 #13：批次 B（245 侧方案可行性侦察，只读）

**新约束（用户确认）**：133 借来的公用机，**完全不能动**；245 可动但独占期
结束后必须复原。因此驱动对齐只能在 245 侧做，且必须可回退。

外部方案候选：① 请平台/同事A 配置 UB 管理面（零改动，优先）；
② 在 245 用 urma_driver 源码构建同源驱动换装（可回退，需内核构建环境）；
③ 换一台与 133 同内核代际的 Target 机（零改动备选）。

本批 = 为 ②③ 采集可行性数据，**全部只读**：

```bash
# a. 内核构建环境（决定方案②能否编译）
ls -l /lib/modules/$(uname -r)/build 2>/dev/null
rpm -qa | grep -iE "kernel-(devel|headers)" 2>/dev/null
ls /usr/src/ 2>/dev/null

# b. 现有驱动模块备份可行性（方案②的回退保障）
for m in ubcore uburma udma ummu ummu_core ubus ubase; do \
  modinfo $m 2>/dev/null | grep filename; done
find /lib/modules/$(uname -r) -name "ubcore*" 2>/dev/null

# c. UB 管理面工具与状态（方案①的可行性）
find /usr /opt -maxdepth 4 -name "*ubmad*" -o -name "*ubtool*" 2>/dev/null | head
systemctl list-units 2>/dev/null | grep -iE "ub|urma|mad" | head
ls /dev | grep -iE "ub|mad" | head

# d. 245 的 URMA 设备与 EID 现状（供管理面配置参考）
cat /sys/class/udmac*/ueid 2>/dev/null | head
find /sys -name "*eid*" 2>/dev/null | head -10
```

**回传**：全部输出追加第 9 节，注明「批次 B 完毕」。本批不做任何
模块装卸/替换/配置变更。

### 指令 2026-09-20 #12：暂停验证，特环境对齐后恢复

**状态：验证批次暂停。** 阻塞是基础设施问题（URMA 驱动版本不一致 +
UB 管理面未互通），SPDK/代码侧无能为力，已升级至内部协调（同事A/平台侧）。

- 测试 Agent 待命，不做任何操作，等待新指令
- 可选（仅在需要补升级材料时执行，只读）：若外部要求 urma_perftest
  复现确认，按指令 #9 的 E2 流程执行并回传
- 恢复条件：133/245 的 URMA 驱动对齐（同一套构建）且管理面配置互通后，
  外部下发恢复指令，从批次 A'' 预案继续（无需重做编译）

### 回执导入 2026-09-20 #6（批次 A' 结果，用户带回）

- 10a ✅：133 编译完成（urma_perf + nvmf_tgt）。解决 6 个依赖问题（numa/
  cunit/uuid/aio/ISAL_DIR/isa-l-crypto 头文件名），全部按「245 系统文件 →
  拷到自家目录隔离使用」模式（/home/lx/nds/{numa,cunit,uuid,aio}_install）；
  configure 实际参数为 --with-isal/--with-isal-crypto（非 --with-isal-install）
- 10b ✅：-h 含 -M npu/npu-staged、-g 支持；CANN 9.1.0
- 10c ❌ **阻塞：245 被他人占用**——p50065573 的 nvmf_tgt（PID 627047，
  /home/p50065573/nof/spdk-urma-v6）在跑且已 vfio 接管 nvme1n1；takeover
  脚本检测到 SPDK 应用即 abort；按隔离守则不能 kill。external 决策：
  **协调独占时间窗（约 15~20 分钟），不做双 target 共存绕行**（URMA 设备
  多进程共存未验证 + 绕过安全检查有风险）
- 10d ✅：内核/驱动对比表已采集——133 内核 6.6.0-155.0.0.143.oe2403sp4
  （发行版标准，比旧 197 的 -159 还不同）、245 = netlab_pcie_ub_compat+；
  URMA srcversion 两台全部不同——**LOC_ACCESS_ERR 风险在新环境依然成立**，
  cpu 回归重点观察
- 卡健康：NPU3/4/5 Alarm（pmbus 供电监控类，非计算核报错）、NPU6/7
  Warning；仅 NPU2 完全可用（-g 2）；NPU0 OK 但有 4.3G 占用残留
- 现场已还原；245 的 p50065573 target 未触碰

### 指令 2026-09-20 #11：批次 A''（等待 245 窗口 + 快速执行预案）

状态：245 被 p50065573 的 nvmf_tgt 占用，用户正在内部协调**独占窗口**。
本批 = 窗口预案。窗口未到时不做任何 245 操作。

> **授权更新（2026-09-20）**：用户已申请到 245 的**暂时独占权**。若执行时
> p50065573 的 nvmf_tgt 仍在运行，**允许 kill 该进程**（kill 后记录 PID 与
> 时间）；其接管的 nvme1n1 若无法简单还原则记录现状（本测试不用该盘）。
> 独占期结束后如实报告本批对机器的全部改动。

**窗口开启的判定**（每次尝试前先确认）：
```bash
ps aux | grep nvmf_tgt        # 无他人进程（或对方明确已让出）
lsblk -o NAME,MOUNTPOINT,TYPE,FSTYPE,SIZE   # nvme1n1 已还原、目标空闲盘仍在
```

**窗口内快速执行预案（全程约 15~20 分钟，按序）：**

```bash
# 1. 起 target（245；空闲盘首选 nvme0n1，被占则依次换 nvme2n1/4n1/5n1/9n1/10n1/11n1）
cd /home/l00955908/nds/spdk
./target_nvme_takeover.sh -d nvme0n1 -L /home/l00955908/nds/UMDK_netlab/lib -i 141.61.84.245 -y

# 2. 133：cpu 回归（重点观察 LOC_ACCESS_ERR 是否复现）
LD_LIBRARY_PATH=/home/lx/nds/spdk/build/lib:/home/lx/UMDK_netlab/lib:/usr/local/Ascend/cann-9.1.0/aarch64-linux/lib64 \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.245 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M cpu -t 5 -g 2

# 3. 133：NDS 全链路首测（npu-staged；观察 507033/LOC_ACCESS_ERR 是否复现）
#    同上命令换 -M npu-staged

# 4. （可选，时间富余）133：-M npu 收集 peermem 路线报错

# 5. 还原现场：245 还原 nvme0n1 + hugepages 恢复；197/133 hugepages 恢复
```

**异常处理**：窗口内任一步失败——记录报错、跳过继续能做的步骤、
窗口结束前恢复现场；全部原样回传。

**回传**：追加第 9 节，注明「批次 A'' 完毕」或「窗口尝试失败详情」。

### 回执导入 2026-09-17（新环境阶段 0~3 + 阶段 4 进行中）

**新环境（第二次换机）**：133 = NPU 机（8× Ascend **950DT** 84GB/卡，
CANN **9.1.0**，与旧环境 950PR/9.0.T500 不同型号与版本）；245 回归当
Target（12× NVMe，nvme8 系统盘）。两台 HEAD 对齐 a3413ce。

- 阶段 0~3 ✅：角色确认（133=Initiator，245=Target）、体检（davinci/hmm
  符号存在、CANN dmabuf 导出能力确认 aclrtMallocPhysical/
  aclrtMemExportToShareableHandle(V2) 等）、代码部署、gds UMDK 部署
  （133=/home/lx/UMDK_netlab，扩展验证齐）
- 阶段 4 ⏳：configure 依赖逐个解决（libfuse3 绕过 --without-nvme-cuse；
  isa-l/isa-l-crypto 在 245 预编译后 rsync 到 133 /home/lx/isal_install；
  外部 isa-l 需 **--with-shared** 重跑 configure），待编译
- 共享机状态：133 有业务（NPU1 vLLM ~78GB、NPU2 python），空闲卡
  0/3/4/5/6/7 可用

### 指令 2026-09-17 #10：批次 A'（新环境编译收尾 + 首测准备）

**10a. 完成编译**（批准用 --with-shared）：
```bash
cd /home/lx/nds/spdk
./configure --with-urma=/home/lx/UMDK_netlab \
            --without-nvme-cuse --with-shared \
            --with-isal-install=/home/lx/isal_install 2>&1 | tail -20
# （若 isa-l 外部安装的参数名与 configure 实际不一致，以 ./configure --help 为准）
nice -n 10 make -j16 2>&1 | tail -30
```

**10b. 产物验证**：
```bash
ls -l build/examples/urma_perf build/bin/nvmf_tgt
LD_LIBRARY_PATH=/home/lx/nds/spdk/build/lib:/home/lx/UMDK_netlab/lib:/usr/local/Ascend/cann-9.1.0/aarch64-linux/lib64 \
./build/examples/urma_perf -h 2>&1 | grep -A3 -- '-M'
# 注意：--with-shared 后运行时需加 <spdk>/build/lib，否则起不来
# 启动 -h 也会打印 CANN runtime loaded: ... —— 确认是 9.1.0
```

**10c. 首测准备与执行（133 共享机硬约束）**：
1. 任何 NPU 测试前：npu-smi info 截图记录，确认所用卡空闲；
   **只允许空闲卡 0/3/4/5/6/7，严禁 NPU1/NPU2**；urma_perf 用 -g <空闲卡序号> 显式指定
2. hugepages 跑前记录/跑后恢复（vLLM 在跑，大页变动影响更大）
3. 245 起 target（同 7b 方式，库路径按本环境调整）：
   ```bash
   cd /home/l00955908/nds/spdk
   ./target_nvme_takeover.sh -d <空闲盘> -L /home/l00955908/nds/UMDK_netlab/lib -i 141.61.84.245 -y
   ```
4. 133 依次跑（traddr 改为 245 的 IP 141.61.84.245）：
   ```bash
   # ① cpu 回归（历史 LOC_ACCESS_ERR 在新环境重验——133 的内核/驱动与
   #    旧 197 不同，此问题可能不复现）
   # ② npu-staged（CANN 9.1.0 + 950DT 上 507033 是否复现同样待验；
   #    历史 HDC remote-jetty 结论基于 950PR+9.0.T500，不可直接搬）
   LD_LIBRARY_PATH=/home/lx/nds/spdk/build/lib:/home/lx/UMDK_netlab/lib:/usr/local/Ascend/cann-9.1.0/aarch64-linux/lib64 \
   SPDK_URMA_MAX_IO_SIZE=4194304 \
   ./build/examples/urma_perf \
     -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.245 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
     -M cpu -t 5 -g <空闲卡序号>     # 先 cpu，通过后同命令换 -M npu-staged
   ```

**10d. 报告要求**：最终报告除常规内容外**必须包含**：两台内核版本与
URMA 驱动 srcversion 对比表（判断旧 LOC_ACCESS_ERR 是否适用本环境）；
CANN runtime loaded 打印行；每次测试前后的 hugepages 与 npu-smi 状态。

**回传**：输出追加第 9 节，注明「批次 A' 完毕」。

### 回执导入 2026-09-15 #5（批次 3R-1 结果，用户带回）

- 环境变更：197 已可联网；197 代码在 /home/lx/spdk（fetch+reset 至 0f4bfaf）；
  151 重新 clone（/home/l00955908/nds/spdk），两台 HEAD 一致
- 7a ✅：151 部署 nds_v1 target（7 个 submodule 全部拉取，configure +
  make -j16 成功，nvmf_tgt/urma_perf 就绪，-h 含 npu/npu-staged）
- 7b ✅ **metadata 问题关闭**：Invalid URMA host metadata 消失，HELLO 通过，
  进入 NVMe Fabrics Connect 数据路径；151 旧 target 正式退役
- 7b 新问题 ⚠️：Connect 失败 **LOC_ACCESS_ERR（status=4）**——target
  pull initiator 内存时 local access error（ureq state=2 PULLING）；target
  侧 timing 显示 W6 register 9.5ms（首次 miss）→ 拉数据失败；显式指定
  DEV_NAME 重试无效
- 7c ❌：507033 未消失——顺序修复无效；测试 agent 诊断：独立 C 程序正常、
  urma_perf 进程内失败，且代码确认修复已生效 → **根因非 main() 内顺序，
  而是 DPDK 链接期构造函数级别的干扰；外部接受此修正**
- 7d ✅：两台现场全部还原

**外部分析（新）：**
1. LOC_ACCESS_ERR 重点怀疑 197 的**发行版标准内核**（6.6.0-159）URMA 驱动
   与同事B 的 gdr 定制内核（6.6.0-gdr_w00921547+，151 在用）行为差异——
   pull 被拒指向 initiator 侧 ummu grant/映射未真正生效
2. 507033 主嫌疑改为**多版本 CANN 混用**（197 有 8.5.0/9.0.1/9.1.0/9.0.T500
   四个版本），独立程序与 urma_perf 可能加载了不同组合；外部已加打印：
   urma_perf 启动时会输出实际加载的 libascendcl 路径
   （CANN runtime loaded: ...）

### 指令 2026-09-15 #8：批次 4（只读诊断批）

前置：批次 3R-1 已回执。本批以只读诊断为主（仅 D1-b/D1-c 需要短暂起
target 复现），遵守隔离守则。197 代码先 git pull 拉取含诊断打印的最新版
并增量重编（nice -n 10 make -j16）。

**D1. LOC_ACCESS_ERR 排查**

a. 驱动版本对比（两台都跑，只读）：
```bash
for m in ubcore uburma udma ummu ummu_core ubus ubase; do \
  echo "== $m =="; modinfo $m 2>/dev/null | grep -E "^(filename|version|srcversion)"; done
```
→ 判断 197（标准内核 6.6.0-159）与 151（gdr 定制内核）的 URMA 模块是否同一套

b. 标准库对照实验（197；需先在 151 用 7b 命令起 target）：
```bash
# 不带 LD_LIBRARY_PATH，强制用系统标准 liburma
# （lazy binding 下 -M cpu 不调 gds 独有符号应能运行；
#  若报 undefined symbol: urma_register_seg_dmabuf 则记录回传——这本身是数据）
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M cpu -t 5
```
→ LOC_ACCESS_ERR 消失 = 锁定「gds liburma × 197 内核」组合问题；
  仍在 = 排除 liburma 因素，矛头指向 197 内核驱动
c. 内核日志（复现 LOC_ACCESS_ERR 的那次）：
```bash
# 151（起 target 后）与 197（跑完测试立即）各抓一次
dmesg | tail -60
```
→ 关注 ummu/udma/ubcore 的 grant/access/map 相关报错

**D2. 507033 排查**

a. 开 CANN 调试日志重跑（最关键，CANN 会自己说出失败原因）：
```bash
ASCEND_GLOBAL_LOG_LEVEL=0 ASCEND_SLOG_PRINT_TO_STDOUT=1 \
LD_LIBRARY_PATH=<与之前完全相同> \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M npu-staged -t 1 2>&1 | head -80
```
b. 实际加载库路径与版本确认：
```bash
ldconfig -p | grep ascendcl          # 系统注册了哪些 CANN
ldd build/examples/urma_perf | grep -iE "ascend|runtime|drv|cann"
# 重编后启动输出会含 "CANN runtime loaded: <路径>" —— 与独立程序用的版本对比
```
c. 加载链核查（如 a/b 仍无头绪）：
```bash
LD_DEBUG=libs ./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M npu-staged -t 1 2>&1 | grep -iE "ascendcl|runtime|libdrv" | head -30
```

**D3. 完成标志**

所有输出原样追加第 9 节，注明「批次 4 完毕」。本批不做任何修复尝试，
外部将根据诊断数据定下批方案。

### 回执导入 2026-09-15 #4（批次 3 结果，用户带回）

- 3a target 启动 ✅：nvme4n1 接管 + nvmf_tgt + RPC 配置完成（脚本从 245 中转
  拷到 151；hugepages 0→2048）
- 3b cpu 回归 ❌：SPDK 层 metadata 交换失败（urma.c:649 Invalid URMA host
  metadata → -71）。**外部归因修正：报错在 SPDK 层而非 liburma——151 的
  target 是同事B 早期构建（7f6f89601 "add urma"），与 nds_v1（urma_modified_v6
  基底）的 HELLO metadata 结构已演进不匹配；修复 = 151 用 nds_v1 重建 target**
- 3c/3d ❌：aclrtSetDevice 在 SPDK 进程内必失败（507033），Python 独立进程
  正常——疑似 DPDK EAL 干扰 CANN 设备映射；**外部修复 = npu 初始化提前到
  spdk_env_init 之前**（urma_perf.c 已改，含 env 失败路径的 fini 清理）
- 3e 恢复现场 ✅：两台 hugepages/盘/进程全部还原
- 3d 未收集到 peermem 注册报错（卡在 ACL 阶段），推迟到问题 2 修复后

### 指令 2026-09-15 #6：解锁批次 3（target 启动 + cpu 回归 + NDS 全链路首测）

（注：本指令内容已由批次 3 实际执行，保留备查；后续以指令 #7 为准）

**3a** node4 接管 nvme4n1 启动 target；**3b** 197 -M cpu 回归；
**3c** -M npu-staged 全链路首测；**3d** -M npu 收集报错；**3e** 恢复现场。

### 指令 2026-09-15 #7：批次 3R-1（151 重建 target 对齐版本 + 复测）

前置：批次 3 已回执。涉及 151（重建 target）与 197（复测）。遵守隔离守则。

**7a. 在 151 部署 nds_v1 target（版本对齐修复）**

```bash
# 在 151 上（目录按实际用户 home 调整，产物只进自家目录）
git clone -b nds_v1 https://github.com/yyyuanhao426-hash/spdk.git /home/<自己>/nds/spdk
# UMDK gds 树：从 197 的 /home/lx/nds/UMDK_netlab 拷（或从 245），放自家目录
scp -r <197>:/home/lx/nds/UMDK_netlab /home/<自己>/nds/UMDK_netlab
grep -c is_gpu_seg /home/<自己>/nds/UMDK_netlab/src/urma/lib/urma/core/include/urma_types.h
cd /home/<自己>/nds/spdk
./configure --with-urma=/home/<自己>/nds/UMDK_netlab 2>&1 | tail -10
nice -n 10 make -j16 2>&1 | tail -30
ls -l build/bin/nvmf_tgt build/examples/urma_perf
git log --oneline -1
```

（若 151 无法访问 GitHub，从 197 scp 代码树过去，勿用 Windows SFTP
——避免 CRLF 问题复发）

**7b. 用新 target 复测 cpu 回归（先 151 起 target，再 197 跑）**

```bash
# 151：用新树的脚本启动 target（脚本在新仓根目录，路径相对自身）
cd /home/<自己>/nds/spdk
./target_nvme_takeover.sh                 # 只分析
cd /home/<自己>/nds/spdk && ./target_nvme_takeover.sh -d nvme4n1

# 197：重跑 cpu 回归（命令同指令 #6 的 3b）
LD_LIBRARY_PATH=/home/lx/nds/UMDK_netlab/lib \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M cpu -t 5
```
- **判定**：若仍报 Invalid URMA host metadata → 记录并回传（说明还有别的
  版本因素，外部继续排查）；若通过预检 → metadata 问题关闭，151 旧 target
  正式退役

**7c. 顺手验证 ACL 顺序修复（197，需 target 已启动）：**

```bash
LD_LIBRARY_PATH=/home/lx/nds/UMDK_netlab/lib \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M npu-staged -t 1 2>&1 | head -40
```
- 观察：若 `aclrtSetDevice failed 507033` 消失 → 顺序修复生效；若通过
  预检 → **这就是 NDS 全链路首测通过**；若仍 507033 → 原样记录回传，
  外部换排查方向

**7d. 恢复现场**（151 还原盘/hugepages，197 hugepages）

**回传**：输出追加第 9 节，注明「批次 3R-1 完毕」。

### 回执导入 2026-09-15 #2（批次 1 结果，用户带回）

**197 体检七项全绿**：A 4× Ascend950PR（128GB HBM/卡）；B CANN 系统级安装
（多版本并存）；C 10+ udmac 设备（拓扑成立）；**D 内核存在 vdavinci_pin_pages /
vdavinci_unpin_pages / hw_vdavinci_pin_page_range（drv_vascend 模块）——Phase 2
内核桥接可行性确认**；E CANN 有 aclrtMemExportToShareableHandle 等导出接口
（是否等价 dma-buf fd 待查证）；F openEuler SP4 aarch64，与 151 延迟 0.3ms。

**编译失败根因**：197 系统 liburma/头文件是标准版，缺 gds 扩展
（urma_seg_cfg_t.is_gpu_seg / urma_register_seg_dmabuf）→ make 失败。
197 可直接 GitHub clone（无 CRLF 问题）；245 的 gds 版 UMDK 树
（/home/yin/gdr/UMDK_netlab）含所需扩展。

**决策**：编译/测试全部改在 197 进行，245 退出（CRLF 问题不再处理）；
gds UMDK 从 245 拷贝到 197 自家目录隔离使用（不动系统库）。

### 回执导入 2026-09-15 #3（批次 2 结果，用户带回）

- gds UMDK 已拷至 197 /home/lx/nds/UMDK_netlab（sshpass 中转），gds 扩展验证
  齐全（is_gpu_seg 字段 + liburma.so 已导出 urma_register_seg_dmabuf）
- configure 成功；SPDK 核心库 + 12 个 bin 全部编译成功（is_gpu_seg 错误消失）
- ❌ urma_perf 编译失败：urma_perf_npu.c 的 npu_driver_init/npu_driver_fini
  定义为 static，与头文件非 static 声明冲突——**外部开发 bug，已修复**
  （commit：fix(nds) static 声明冲突）
- **D 项结论修正（重要）**：vdavinci_pin_pages 等符号在 kallsyms 中均为小写
  t（模块局部），无 __ksymtab——**未导出，Phase 2 内核桥接不能直接调用**。
  Phase 2 方向重估：E 项（CANN 导出接口 → dma-buf 注册路线）重要性上升，
  因 dma-buf 走标准内核框架，无需 davinci 专有符号；待查证点：gds 内核
  udma 驱动的 urma_register_seg_dmabuf 是否已实现 dma-buf import。

### 指令 2026-09-15 #5：批次 2 补丁（拉修复重编 urma_perf）

前置：批次 2 其余部分已完成。本批全部在 **197** 上，遵守隔离守则。

```bash
cd /home/lx/nds/spdk          # 或实际 GitHub clone 目录
git stash list | head -1      # 确认无未保存改动（有则先回传询问）
git pull origin nds_v1        # 拉取 static 声明修复
nice -n 10 make -j16 2>&1 | tail -20   # 增量编译，只重编 urma_perf，很快
ls -l build/examples/urma_perf
./build/examples/urma_perf -h 2>&1 | grep -A3 -- '-M'
```

**完成标志**：urma_perf 编出、帮助文本含 npu/npu-staged → 回传注明
「批次 2 补丁完毕，等待解锁批次 3」。若 make 仍失败，完整报错回传。

### 指令 2026-09-15 #4：解锁批次 2（在 197 搭建 gds 编译环境）

前置：批次 1 已确认。本批全部在 **197** 上操作，遵守 0.5 节隔离守则。
245 上的 CRLF 修复任务作废。

**2a. 从 245 拷贝 gds 版 UMDK 树到自家目录（只读 245）：**

```bash
# 在 197 上执行（若 197 与 245 ssh 不通，改由用户中转拷贝）
mkdir -p /home/l00955908/nds
scp -r /home/yin/gdr/UMDK_netlab /home/l00955908/nds/UMDK_netlab
```

**2b. 验证拷贝物含 gds 扩展（只读）：**

```bash
# 头文件应有 is_gpu_seg 字段
timeout 30 find /home/l00955908/nds/UMDK_netlab -name "urma_api.h" | head -3
grep -rn "is_gpu_seg" /home/l00955908/nds/UMDK_netlab/src/urma/lib/urma/core/include/ | head -5
# 库应有 urma_register_seg_dmabuf 导出
nm -D /home/l00955908/nds/UMDK_netlab/lib/liburma.so | grep -E "register_seg"
```

**2c. 重新 configure + make（197 上 GitHub clone 的 spdk 目录）：**

```bash
cd /home/l00955908/nds/spdk   # 或实际 clone 目录
git log --oneline -3           # 应看到 cc53791/3bf02d6/e1b585f/ddbd44a 系列
./configure --with-urma=/home/l00955908/nds/UMDK_netlab 2>&1 | tail -20
nice -n 10 make -j16 2>&1 | tail -50
ls -l build/examples/urma_perf
./build/examples/urma_perf -h 2>&1 | grep -A3 -- '-M'
```

**2d. 【新增，只读】补齐 D/E 两项的细节（Phase 2 设计输入）：**

```bash
# D 细节：drv_vascend 模块的 pin 符号是否对外导出（T=导出 / t=局部）
timeout 60 find /lib/modules/$(uname -r) -name 'drv_vascend*' 2>/dev/null | head -3
nm <上面找到的 .ko 路径> 2>/dev/null | grep -i vdavinci | head -10
# E 细节：CANN 导出接口完整签名线索
nm -D /usr/local/Ascend/ascend-toolkit/latest/lib64/libascendcl.so 2>/dev/null \
  | grep -iE "Export|Shareable|MallocPhysical|ImportFrom" | head -20
#（若 latest 链接不存在，用批次 1 找到的实际版本路径）
```

**完成标志**：urma_perf 编出且帮助文本含 npu/npu-staged → 回传注明
「批次 2 完毕，等待解锁批次 3」。若 make 仍失败，完整报错回传等待指令。

**提示**：之后所有 urma_perf 运行都带
`LD_LIBRARY_PATH=/home/l00955908/nds/UMDK_netlab/lib`（隔离加载 gds liburma，
不动系统库）；gds liburma 与 197 内核驱动的兼容性将在批次 3 首次连接时验证。

### 指令 2026-09-15 #3：改分批制，本轮只做批次 1

外部确认：任务粒度太大，改为**分批执行、每批回传、确认后解锁下一批**。
本条指令**取代**之前指令中的执行节奏（各批次内容不变）：

| 批次 | 内容 | 风险 | 解锁条件 |
|---|---|---|---|
| **批次 1（本轮）** | 197 节点体检 A~F 七项（指令 #2 清单，全部只读） | 零风险 | 即刻执行 |
| 批次 2 | 修 CRLF + 编译 urma_perf（指令 #1 任务 1） | 低（产物在自家目录） | 批次 1 回传确认后 |
| 批次 3 | target 启动 + -M cpu 回归 + 错误路径（任务 2/3） | 中（接管 151 的盘） | 批次 2 回传确认后 |
| 批次 4 | 恢复现场（任务 4） | 低 | 批次 3 完成 |

**本轮你只做**：指令 #2 的 NPU 节点体检 A~F 七项（含 B2），原样记录全部输出，
追加到第 9 节回执区后回传，注明「批次 1 完毕，等待解锁批次 2」。
**不要**开始批次 2/3 的任何操作（包括修 CRLF、编译、接管盘）。

### 指令 2026-09-15 #1（已改分批制，内容保留备查）

背景说明：这两台是同事B 的 GDS 测试机（151/245），无 NPU 属实。本轮目标改为
**「代码质量验证」**：修编译 → CPU 回归 → 错误路径。NPU 相关测试等真正的
昇腾机器到位后再做（用户正在协调）。

**任务 1：修复 CRLF 并完成编译（node1 = 141.61.84.245）**

```bash
# 1a. 先测 GitHub 连通性
curl -sI --max-time 10 https://github.com | head -3
```

- **若通**：换到新目录重新 clone（顺带解决 git 历史问题）：
  `git clone -b nds_v1 https://github.com/yyyuanhao426-hash/spdk.git /home/l00955908/nds/spdk-git`
  然后在新目录编译；验证 `git log --oneline -3` 应看到 e1b585f / ddbd44a。
- **若不通**：在现有目录转换行尾（file 检测只转 CRLF 文件，不动二进制）：
  ```bash
  cd /home/l00955908/nds/spdk
  git ls-files -z | xargs -0 file | grep CRLF | cut -d: -f1 | xargs -r sed -i 's/\r$//'
  ```

然后编译并验证：

```bash
./configure --with-urma=/home/yin/gdr/UMDK_netlab 2>&1 | tail -20
make -j$(nproc) 2>&1 | tail -50
ls -l build/examples/urma_perf
./build/examples/urma_perf -h 2>&1 | grep -A2 -- '-M'
```

预期：configure/make 成功；帮助文本中出现 npu / npu-staged 取值。

**任务 2：-M cpu 回归（先启动 target）**

2a. 在 **node4（target，151）**：先只分析不接管：
```bash
cd /home/xxx/spdk && ./target_nvme_takeover.sh        # 只分析，记录候选盘
cd /home/xxx/spdk && ./target_nvme_takeover.sh -d nvme4n1   # 用空闲盘接管+启动 nvmf_tgt
```
（接管前核对 nvme4n1 确为空闲盘；若脚本分析结果显示异常，停下回传等待指令）

2b. 在 **node1（initiator，245）**：先确认本机 UMDK lib 路径存在：
```bash
ls /home/tong/umdk_gpu_isolated/lib /home/yin/gdr/UMDK_netlab/lib 2>/dev/null
```
然后先查本机 URMA 设备名（同事B 脚本里的 udmac1d1e2 是旧 initiator node3 的，
node1 可能不同）：
```bash
find /sys -name '*udmac*' 2>/dev/null | head
ls /dev | grep -i udma
```
再跑回归（先不带 SPDK_URMA_DEV_NAME 让其自动选第一个设备；如连接失败再
补上 DEV_NAME 重试）。**遵守隔离守则**：跑前记录 hugepages 现状
（`grep -i huge /proc/meminfo`），默认 -T 1 只绑 1 核无需 -m，勿加大线程数：
```bash
LD_LIBRARY_PATH=<上面存在的 umdk lib 路径> \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M cpu -t 5
```
预期：`Preflight host WRITE + READ verification passed` + 带宽/延迟输出。

**任务 3：错误路径验证（node1，不需要 target）**

```bash
./build/examples/urma_perf -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.151 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' -M npu -t 1
./build/examples/urma_perf -r '...' -M npu-staged -t 1
```
预期均报 `Unable to load libascendcl.so`（无 CANN 环境优雅失败）——
**原样记录报错**，这是错误处理路径的验证。

**任务 4：恢复现场**

- node4：测试后还原 NVMe 盘（脚本有残留还原功能；若不确定还原方法，
  记录盘的当前状态并注明"等待还原指令"，不要自行操作）
- node1/197：确认 hugepages 恢复到跑前水平（对照任务 2b 的记录），
  清理自己目录下的临时文件，他人目录与系统目录不动

**回传要求**：全部输出追加到本文件第 9 节新条目，commit
（`docs(nds-task): CRLF修复+编译+cpu回归+错误路径验证`）并 push；
若机器无法访问 GitHub，把追加后的本文件全文交给用户带回。

### 指令 2026-09-15 #2（外部 AI → 内部 AI）：新增 NPU 节点体检

背景：用户确认公司有一台昇腾 NPU 节点（唯一一台），计划作为 NDS 的
Initiator；Target 继续用 node4/151。本轮在完成上一轮指令的同时，
**优先**对这台 NPU 节点做环境体检（它能回答 Phase 2 的两个关键问题）。

**NPU 节点体检清单（在该节点上逐条执行并原样记录）：**

```bash
# A. NPU 基础
npu-smi info

# B. CANN 是否安装（限定路径，禁止全盘 find）
timeout 60 find /usr/local/Ascend /usr/lib64 /opt /home -name "libascendcl.so" 2>/dev/null
ls /usr/local/Ascend 2>/dev/null

# B2. UMDK/liburma 是否可用（编译 urma_perf 必需；限定路径）
timeout 60 find /usr/local /usr/lib64 /opt /home -name "urma_api.h" -not -path "*/spdk*" 2>/dev/null
ldconfig -p | grep urma
ls /usr/lib64/liburma* 2>/dev/null

# C. 【决定成败】URMA 网卡是否存在
find /sys -name '*udmac*' 2>/dev/null | head
ls /dev | grep -iE "udma|ub"
lsmod | grep -E "udma|urma|ubus|ubcore|ubase"

# D. 【Phase 2 关键】内核 davinci pin 符号
cat /proc/kallsyms | grep -i davinci | head -50
cat /proc/kallsyms | grep -iE "hmm_|davinci.*pin|pin.*davinci" | head -30

# E. 【Phase 2 关键】CANN dmabuf 导出能力（用 B 的路径代入）
nm -D <libascendcl.so 路径> | grep -iE "dmabuf|handle|fd|export" | head -40

# F. 内核/系统与网络连通性
uname -r
cat /etc/os-release | head -3
uname -m
ip a | grep "inet "
# 与 node4/151 连通性（TCP + ping）
ping -c 3 141.61.84.151
```

**判定标准（写入报告结论）：**
- A~C 全部正常 → NPU 节点可直接当 Initiator，NDS 全链路测试可排期
  （拓扑定为 197=Initiator + 151=Target；245 仅在本轮 CPU 回归中当编译机，
  NDS 正式测试不再需要）
- C 无 URMA 设备 → 硬件缺口，回报"需协调 URMA 网卡"，其余照常交付
- B2 无 UMDK/liburma → 编译缺口，可在 197 上装 UMDK 或把 245 编好的
  二进制拷贝过去，回报时注明选择哪种方式
- D/E 的输出是 Phase 2 内核桥接模块设计的直接输入，务必原文记录
