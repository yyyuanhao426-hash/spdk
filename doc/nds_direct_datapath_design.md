# NDS 直连方案：数据链路与技术选型定稿

> 状态：方案定稿（方向性文档） ｜ 配套：[nds_direct_integration_design.md](nds_direct_integration_design.md)（v2 模块实施设计）
> 依据：P2-16~P2-23 批次实测结论（133）＋ CCDK（同事C 仓）权威参数提取
> 本文档回答三个问题：数据链路长什么样、每个部分参照了谁、为什么这样选。

## 1. 参照谱系（四位同事各贡献了什么）

| 同事 | 产出形态 | 解决的问题 | 在 NDS 中的角色 |
|------|---------|-----------|----------------|
| **同事A** | GDR/GDS 机制**调研文档**（HaoyuWang2001/GDR-GDS，无代码） | "为什么"——GPU 直存的原理与难点（pin 物理页→SGT→IOMMU 映射的机制链） | 认知输入层：Phase 2 机制推演的最初来源，无代码组件 |
| **xingtong** | 内核补丁（urma_driver GDR 分支，代码） | GPU 显存注册为 URMA 内存的**内核框架**（is_gpu_seg → nvidia_p2p pin → UMMU 映射） | 组件参照：注册语义参照 + npu_bridge 备用方案模板 |
| **同事B** | SPDK 传输层（urma_modified_v6，代码） | **存储栈的 URMA 化**：NVMe-oF over URMA 完整链路 | 组件复用：存储栈骨架原样采用（Phase 1 基底） |
| **同事C** | CCDK 完整实现（内网仓，代码） | NPU HBM 注册（RA/HCCP）＋ UB 原生盘端点（SSU） | 组件复用：HBM 注册方式（Module A 即照其 nds_init_ra 实现）＋ 模块 B/C 权威参数 |

一句话：同事A 回答"为什么"，xingtong 回答"内核怎么实现"，同事B 回答"存储栈怎么搭"，同事C 回答"NPU 上怎么绕开内核做"。

## 2. 核心技术选型：为什么走用户态 RA/HCCP，不走内核补丁

**问题本质**：让 URMA 网卡能 DMA 到设备内存，有两条路——

| 步骤 | 内核补丁路（xingtong，GPU） | 用户态 RA 路（我们，NPU） |
|------|---------------------------|--------------------------|
| pin 服务由谁提供 | NVIDIA 驱动（nvidia_p2p_get_pages，官方导出） | 华为 RA 库（libra.so，官方用户态库） |
| 注册逻辑在哪执行 | **内核**（补丁过的 udma.ko） | **用户态**（libra.so，内部与官方驱动栈协商） |
| 需要动内核吗 | 要：改 urma 内核驱动 + 编译 + 装载 .ko | 不要：dlopen 一个库 |
| 机器上要满足什么 | 内核开发环境 + 驱动源码版本匹配 + 装卸模块权限 | CANN 在位即可 |

**NPU 侧选择用户态路的原因**（不只是权限问题，是接口缺失）：

1. **没得抄**：xingtong 框架要求设备驱动栈提供 nvidia_p2p 那样的 pin 接口；昇腾驱动栈不提供——照他的模式写的 npu_bridge 无接口可调（P2-8 实测）。
2. **改不动**：自造接口需先改 URMA 内核驱动 udma.ko；133 运行版未编入 gpu_p2p 框架（ENABLE=0），且驱动源码与内核版本错配编不过。
3. **不必改**：华为在 libra.so 留了官方用户态口子（RaCtxLmemRegister nonPin），注册效果经 P2-19/P2-21 双向实测与内核路等价（远端 jetty 逐字节读写一致）。

> 环境注记：已获独立使用 NPU 节点，系统级操作约束解除——但内核路缺的是
> NPU 驱动侧接口而非操作权限，路线判断不变。独立节点的价值：URMA 回环
> 问题排查、自由部署、性能测试窗口。

## 3. 数据链路四图（各参照方案 vs 我们的方案）

### 3.1 xingtong（GDR 内核补丁）——解决"GPU 显存怎么让网卡摸到"

```
      GPU 节点（他的场景）                    存储节点
   ┌──────────────────────┐             ┌──────────────────┐
   │   ┌─────┐            │             │    ┌─────┐       │
   │   │ GPU │ 显存        │             │    │ cpu │       │
   │   └──┬──┘            │             │    └──┬──┘       │
   │       │ PCIe         │             │       │ NVMe     │
   │   ┌──┴──┐            │             │    ┌──▼──┐       │
   │   │ cpu ├────────────┼─────UB─────►│    │ ssd │       │
   │   └─────┘            │             │    └─────┘       │
   └──────────────────────┘             └──────────────────┘
```

补丁在内核 udma.ko：注册时调 nvidia_p2p 把显存物理页 pin 住、写进
UMMU 映射 → 网卡能直接 DMA 显存。不做传输协议、不做存储栈。

### 3.2 同事B（SPDK GDS）——解决"存储栈怎么用 URMA 传输"

```
      发起节点                              存储节点（target）
   ┌──────────────────────┐             ┌──────────────────────┐
   │  应用内存(host/GPU)    │             │   ┌─────┐            │
   │      ▲               │             │   │ ssd │            │
   │      │ ② URMA 注册    │             │   └──▲──┘            │
   │   ┌──┴──┐            │             │      │ ④ SPDK 用户态  │
   │   │ cpu │ ①initiator │             │      │    NVMe 驱动   │
   │   │     │  传输层      │             │   ┌──┴──┐            │
   │   └─────┘            │             │   │ cpu │ ③import_seg │
   │    [URMA 网卡]────────┼─────UB─────►│   │     │  +jetty拉/推 │
   └──────────────────────┘             │   └─────┘            │
                                        │  （host 内存 iobuf）   │
                                        └──────────────────────┘
```

①②③④ 整条软件链他都有：initiator 传输层（连接/capsule/qpair）、
target 导入对端内存、target jetty 主动拉/推、SPDK 落盘。不解决设备
显存注册（GPU 需 xingtong 的内核框架）。存储节点数据落一次 host 内存。

### 3.3 同事C（NPU Direct SSU）——解决"NPU HBM 注册 + UB 原生盘端点"

```
      NPU 节点                              SSU（带 UB 口的盘）
   ┌──────────────────────┐             ┌──────────────────────┐
   │   ┌─────┐            │             │  ┌────────────────┐  │
   │   │ NPU │ HBM        │             │  │ SSU 盘 = UB 端点│  │
   │   └──┬──┘            │             │  │ （盘自带 eid）   │  │
   │       │ UB（SoC 集成， │             │  └────────▲───────┘  │
   │       │ 非 PCIe）      │             │           │          │
   │   ┌──┴──┐            │             │      UB 直接可达      │
   │   │ cpu ├────────────┼─────UB─────►└──────────────────────┘
   │   └─────┘            │
   └──────────────────────┘
```

两个贡献：① HBM 注册——RA/HCCP（libra.so，纯用户态
RaCtxLmemRegister nonPin）；② 存储端点选 UB 原生盘 SSU——盘自己是
UB 端点，HBM 数据经 UB 直达盘，全程不过 host 内存（nvme_nds 内核
驱动把 {HBM VA, 盘物理块} 直交内核 DMA）。

### 3.4 我们的 NDS（NPU Direct SSD）——B 的骨架 + C 的注册 + 新增胶水

```
      NPU 节点（initiator）                 存储节点（target）
   ┌──────────────────────┐             ┌──────────────────────┐
   │   ┌─────┐            │             │   ┌─────┐            │
   │   │ NPU │ HBM        │             │   │ ssd │            │
   │   └──┬──┘            │             │   └──▲──┘            │
   │       │ ①RA/HCCP 注册  │             │      │ ④ SPDK 用户态  │
   │       │（同事C 的拼图）  │             │      │    NVMe 驱动   │
   │   ┌──┴──┐            │             │   ┌──┴──┐            │
   │   │ cpu ├────────────┼─────UB─────►│   │ cpu │ ②③同事B 的  │
   │   └─────┘ UB（950原生）│             │   │     │ import+jetty│
   └──────────────────────┘             └──────────────────────┘
                              ▲
                              │ 段信息 {eid,uasid,va,token} 经 TCP
```

**组成部分映射表**：

| 我们的组成部分 | 参照谁 | 状态 |
|--------------|--------|------|
| ① HBM 注册为可远端 DMA 的内存 | 同事C（RA/HCCP） | ✅ Module A 完成（P2-22 快验通过） |
| ②③ initiator 传输层 + target import/jetty 数据面 | 同事B（骨架原样复用） | ✅ 已存在（lib/nvme/nvme_urma.c + lib/nvmf/urma.c W5） |
| 段信息从 RA 产出合成 urma_seg_t（胶水） | 新增（模块 B，参数照同事C nds_get_segment_info） | 🔨 待开发 |
| NPU 内存分流（跳过 is_gpu_seg 注册） | 新增（模块 C） | 🔨 待开发 |
| ④ 存储节点落盘 | 同事B（SPDK） | ✅ 已存在 |

**与三位的本质差别**：

- vs **xingtong**：他靠内核补丁让 GPU 显存可达；我们用用户态 RA/HCCP
  让 HBM 可达（NPU 没有 nvidia_p2p 等价接口，内核路缺的是接口不是权限）。
- vs **同事B**：他的 initiator 内存是 GPU/host；我们换成 NPU HBM，
  注册方式与段信息来源要改（即模块 A 扩展 + B/C 的全部工作量）。
- vs **同事C**：共享 HBM 注册技术（同一条路、同一个库、同一组调用）；
  分歧在数据面与端点——他用专用内核驱动 nvme_nds 搬运、端点是 UB 原生
  SSU（可零中转）；我们用 SPDK 通用栈、端点是标准 NVMe SSD（存储节点
  落一次 host 内存）。**从 HBM 到标准 NVMe SSD 的那段路他没走——正是
  同事B 骨架覆盖的部分。**

## 4. 数据链路五步（端到端时序）

| 步骤 | 发生在 | 内容 | 实测依据 |
|------|--------|------|---------|
| ① | NPU 节点 | RA/HCCP 把 HBM 注册为可远端 DMA 的段（libra.so，纯用户态） | P2-19/P2-21 |
| ② | 两节点间 | TCP 控制面把段信息 {eid,uasid,va,token} 发给 target（capsule 携带 urma_seg_t，同事B 骨架已有） | 代码确认 |
| ②' | 存储节点 | urma_import_seg 把 HBM 段映射进 target 的 URMA 域 | P2-19 |
| ③ | 两节点间 | UB 数据面：target jetty 主动 READ（写盘）/WRITE（读盘），直达 HBM，initiator 零参与 | P2-19/P2-21 |
| ④ | 存储节点 | iobuf（host 内存）↔ SSD：SPDK 用户态 NVMe 驱动（VFIO 轮询，绕内核走 PCIe） | Phase 1 |

架构语义：**数据搬运由 target 发起**（推变拉），initiator（NPU 推理
进程）零参与——直连相对 npu-staged 中转的核心收益。

## 5. 三个关键问题的最终答案（均有实测依据）

**Q1：NPU direct 完全不经过 CPU 吗？——是，软件层与物理层双重确认。**

| 含义 | 答案 | 依据 |
|------|------|------|
| 数据是否落入 CPU 内存/被软件拷贝 | 否 | P2-19/P2-21：远端 jetty 直达 RA 注册的 HBM，逐字节一致，无 host RAM 落地 |
| 数据是否穿过 CPU 的 PCIe 控制器 | 否 | P2-23 D 项：950DT NPU 非 PCIe 卡（/sys/bus/ub，npu-smi topo 全 UB，SoC 集成），HBM 数据走 UB 总线 |

**Q2：NPU 节点 ↔ storage 节点走 URMA 吗？——是。**
数据面 = URMA jetty(CTP) over UB 网络（单机回环已实测，跨节点同一协议栈）；控制面（HELLO/capsule 段信息）= TCP。与同事B 的 GDS 同构。

**Q3：存储节点上 URMA 怎么访问 SSD/SSU？差别？**

| 端点 | 访问方式 | 是否过 host RAM |
|------|---------|----------------|
| SSU（UB 原生盘，同事C） | 盘自己是 UB 端点，可被 URMA/UB 直接访问 | 理论零中转（其 nvme_nds 数据面已证实 HBM↔盘直达） |
| 标准 NVMe SSD（我们） | SSD 是 PCIe 设备，URMA 网卡无法直接 DMA | 必须一次中转：target 把 iobuf（host RAM）注册为 URMA 内存，数据落 iobuf 后由 SPDK 用户态 NVMe 驱动写盘 |

结论：NDS 在存储节点有一次 host RAM 落地——与 GDS 完全一样，是
NVMe SSD 生态的固有约束（盘不是 UB 端点），不是方案缺陷。直连省掉
的是 **NPU 节点侧**的 D2H 中转与 initiator CPU 参与。

## 6. 环境认知备忘（P2-23 修正）

- **133 上不存在真 UB-SSU 硬件**：nvme1n1（/home/ssu/ramdisk）实为
  TCP 回环 nvmf 逻辑盘（127.0.0.1:4421，subnqn=tcp_loopx3）。
  P2-17/P2-18 直连验证的存储端点是软件回环盘；机制链（RA 注册 + UB
  传输 + 数据一致）成立不变，但对外表述需准确。
- 133 的 URMA 回环握手 rc=-5（-M cpu 对照同败，非我方回归）——V2/V3
  回环验证的拦路虎，待独立节点环境排查。
- 950DT 拓扑事实：lspci 无 ascend/davinci PCIe 设备；NPU 位于
  /sys/devices/virtual/devdrv-class/davinci0..7；UB 独立总线
  /sys/bus/ub/devices/（ub_bus_controller0/1，udmac0/1d1e2..e6）。

## 7. 实施衔接

模块实施设计（模块 A/B/C/D 改动点、验证计划 V1-V3、风险表）见
[nds_direct_integration_design.md](nds_direct_integration_design.md)。
本文档定稿后，v2 的一处简化生效：**模块 B 不再需要新消息类型**——
同事B 的 capsule 已携带 urma_seg_t{eid,uasid,va,len,attr,token_id}，
NPU 内存只需在 initiator 侧合成该结构（token_id 用 RA lmem 注册输出，
段导入参数照同事C：SEG_NOMAP + R|W|A + seg token=0xACFE + jetty
CTP token=0）。
