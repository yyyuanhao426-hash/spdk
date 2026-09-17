# NDS 测试 Agent 入职必读（新建 Agent 第一份文档）

> 你是 NDS 项目的测试 Agent。本文档包含接手工作所需的全部稳定信息。
> 读完本文 → 去主对话文件取【最新指令】→ 执行。不需要重喂项目历史。

## 1. 项目目标（一段话）

**NDS（NPU Direct Storage）**：让昇腾 NPU 的 HBM 显存绕过 CPU 内存、
经 URMA 总线直连远端 NVMe 盘。这是同事B 已验证的 GPU 版（GDS，
Tesla V100）的 NPU 复刻。测试工具是 SPDK 的 `urma_perf`，我们为其新增了
NPU 内存路线（`-M npu` / `-M npu-staged`）。

```
NPU 节点（Initiator）                Target 节点
NPU HBM ──► [host DRAM 暂存] ──网卡──URMA──► iobuf ──► NVMe SSD
             └─ 中转路线 B；去掉中转 = 直连路线 A（Phase 2）
```

## 2. 协作机制（怎么和外部开发 AI 配合）

- **主对话文件**：`nds_company_validation_task.md`（本目录）
  - 第 10 节「外部 AI 指令区」：**你的任务以最后一条指令为准**
  - 第 9 节「内部 AI 回执区」：把每步的**全部原始输出**追加到这里
    （成功和报错都要，不要总结改写，不要删改已有内容）
- **分批制**：每批做完 → 回传 → 外部确认 → 解锁下一批。禁止跳批。
- **commit 规范**：conventional commits，如 `docs(nds-task): 批次N回执`
- **回传方式**：能联网就 git push；不能则把文件全文交给用户带回

## 3. 新 Agent 上手三步

```
① 读本文（10 分钟）
② 打开主对话文件第 10 节，找到最后一条指令——那就是你现在的任务
③ 按指令执行；若指令引用了环境 SOP，走 env_sop.md
```

## 4. 当前环境速览（⚠️ 易变节：换环境时由外部 AI 更新本节）

> **最近一次更新**：2026-09-17，第二次换环境（197/151 → 133/245；
> 原 950PR 机器被占用）。

| 项 | 值 |
|---|---|
| NPU 节点（Initiator） | **133**：8× Ascend **950DT**（84GB HBM/卡，与旧环境 950PR 不同型号）；CANN **9.1.0**（/usr/local/Ascend/cann-9.1.0/aarch64-linux/lib64）；内核 davinci/hmm 符号存在；**共享机且有业务在跑**（NPU1 vLLM、NPU2 python）——测试只用空闲卡 0/3/4/5/6/7，用 -g 显式指定 |
| Target 节点 | **245**：12× NVMe 7.68T；nvme8=系统盘（禁碰），nvme7/3=raid 成员，其余空闲 |
| 代码路径 | 133：/home/lx/nds/spdk（HEAD a3413ce）；245：同仓 clone |
| UMDK（gds 版）路径 | 133：/home/lx/UMDK_netlab；245：/home/l00955908/nds/UMDK_netlab |
| 两台互通 | ✅ ping 正常 |
| 编译注意 | isa-l/isa-l-crypto 用 245 预编译外部安装（/home/lx/isal_install），configure 需加 **--with-shared**；运行时 LD_LIBRARY_PATH 需加 <spdk>/build/lib |

## 5. 运行 urma_perf 的固定姿势（gds liburma 隔离加载）

```bash
LD_LIBRARY_PATH=<gds UMDK lib 目录>:<CANN lib64 目录> \
SPDK_URMA_MAX_IO_SIZE=4194304 \
./build/examples/urma_perf \
  -r 'trtype:URMA adrfam:IPv4 traddr:<Target IP> trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  -M <cpu|npu|npu-staged|posix|peermem|dmabuf> -t 5
```

注：经 dlsym 改造后，同一二进制也可用系统标准 liburma 运行
host-only 路线（cpu/posix/npu-staged）——这是标准库对照实验的基础。

## 6. 隔离守则摘要（全文见主对话文件 0.5 节）

共用机上：产物只进自家 home；find 限定路径+timeout；编译
`nice -n 10 make -j16`；hugepages 跑前记录跑后恢复；默认 -T 1 绑核从少；
URMA 设备先确认无他人使用；**严禁装卸内核模块**；覆写只允许对已确认
空闲盘；影响他人立即停止回传。

## 7. 踩坑清单（前人经验，务必读）

1. **CRLF**：代码从 Windows 经 SFTP/文本方式上传会带 \r，configure 必挂。
   一律 git clone 或 Linux 间 scp。
2. **编码**：md 文件是 UTF-8，不要用会转码的工具改写（历史事故：整文件
   中文变乱码）。
3. **liburma 两个版本**：系统自带的是标准版（无 is_gpu_seg/
   urma_register_seg_dmabuf 扩展）。编译必须用 gds 版；运行时 host-only
   路线两种都可以（dlsym 兼容），但 NPU 直连注册必须 gds 版。
4. **URMA 设备名**：机器上可能有 10+ udmac 设备，先 `find /sys -name '*udmac*'`
   记录；连接失败再用 SPDK_URMA_DEV_NAME 指定重试。
5. **hugepages**：SPDK 需要大页。跑前 `grep -i huge /proc/meminfo` 记录，
   跑后确认恢复。
6. **ACL 初始化顺序**：npu 初始化在代码里已置于 spdk_env_init 之前
   （DPDK 环境会干扰 CANN），不要"顺手"改回去。

## 8. 文档地图

| 文档 | 用途 |
|------|------|
| 本文（onboarding.md） | 入职必读（稳定） |
| env_sop.md | 换新机器时的环境搭建 SOP（稳定） |
| nds_company_validation_task.md | 主对话文件：回执 + 指令（易变） |
| ../nds_design.md | 技术设计文档（三层架构） |
