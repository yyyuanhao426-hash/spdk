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
