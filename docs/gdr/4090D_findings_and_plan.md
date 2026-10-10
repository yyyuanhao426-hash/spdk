# 4090D GDR peermem 两轮排查总结与后续方案（2026-10-10）

> 定位：**记录文档** —— 把两轮排查（第一轮：运行时实验 + 用户态源码；第二轮：内核源码）做了什么、
> 发现了什么、得出什么结论汇总成一份可独立转发的记录，并给出之后的解决方案。
> 关联：证据层《4090D_V100_peermem.md》、结论层《4090D_roadmap.md》、源码核查《4090D_source_audit.md》、
> V100 基线《analysis.md》（均在 `/home/senorita/nof/docs/spdk/gdr/`）。
> 环境：SPDK `urma_enable_v2` / UMDK liburma / UB fabric；node1=4090D（GeForce，hook 路线）、
> node4=V100（原生 peermem）、node2=A100（硬件兜底）。

---

## 0. 一页版结论

1. **4090D 已从 ~1 MiB/s 修到 16.6 GB/s**（整池注册 `URMA_PERF_REGION_REG=1` + `-o 4M -T4 -b8`），
   已超 V100 原生 peermem 的 13.4 GiB/s；目标 20+，pinned DtoH 链路天花板 24.2 GB/s。
2. **SPDK 侧无问题**（两轮审查确认，代码已全部读过）。剩余 ~7.6 GB/s 差距全部在
   UMDK/UMMU 的 GPU 内存翻译路径，且第二轮内核源码核查把病灶**全部定位到 file:line**——
   其中两个是调试代码，一个是 bridge 未合并：
   - verify 回读 = 调试补丁（`core_matt.c:194-206`，`//TONG ADD FOR DEBUG`）；
   - 每 entry 无条件 pr_info → printk console 串行 = 注册争用主嫌（`core_matt.c:180/196`）；
   - bridge 每 64KB 一条 sg 不合并，而 UMMU 本来就收变长条目（`udma_nv_p2p_bridge.c:218-239` / `core_matt.c:160-210`）。
3. 病灶都不在 UMMU 核心设计里，**不等 UMDK 就能动手**：删调试、降日志、bridge `coalesce=2m`。
   第二墙（footprint 256MB 塌缩）是 2MB 合并的大奖赌注：256MB 从 4096 条降回 128 条。

---

## 1. 问题与软件栈（背景）

**症状**：4090D（GeForce，无原生 nvidia_p2p，靠 LD_PRELOAD hook 伪造 attr116/VMM 分配/P2P 注册）
SPDK 走 peermem 模式带宽崩塌（最初 ~1 MiB/s）；同机 cpu（host 内存）路线正常。

**数据面注册栈**（两轮核查后完整钉死）：

```
SPDK spdk_nvme_urma_register_memory（4K 对齐，is_gpu_seg=1）
→ liburma udma_u_register_seg = ummu_grant（MAPT token/perm）+ urma_cmd_register_seg
→ udma.ko udma_gpu_pin_seg_pages
→ udma_nv_p2p_bridge.ko → nvidia_p2p_get_pages（恒 64KB 页）
→ ummu_sva_matt_map_phys_sg（kernel_ummu/ummu-core/core_matt.c）
→ 逐 sg entry：iommu_map（arm_lpae 页表，隔离 SVA domain，udma_ctx.c:116）
```

---

## 2. 第一轮：运行时实验 + 用户态源码（做了什么、发现什么）

### 2.1 实验记录（全部 node1，原始数据见《4090D_V100_peermem.md》）

| 实验 | 做法 | 结果 | 回答了什么 |
|---|---|---|---|
| Step 0 平台基线 | pinned cudaMemcpy DtoH | **24.2 GB/s** | 链路健康，gen4 x16 满速 |
| Step 1b/1c 注册缓存验证 | 4K 小块命中统计；`-t 900` 超 128 槽 wrap | 4K 命中 358 万/70 miss；wrap 后 4MB 命中 191 万、稳态 8.5 GB/s | 注册缓存无罪——"4MB 永不命中"是**饥饿伪象** |
| Step 1d 注册税测量 | per-IO 注册路线打点 | ~181ms 固定 + ~108ms/64KB entry（争用下）；无争用 0.06ms/entry | 慢的根因 = **注册税**，成本强上下文相关 |
| Step 1e 整池注册（P1） | `URMA_PERF_REGION_REG=1` 整池一次注册 | setup 0.2s 完成；稳态 8.5 → **16.6 GB/s** | 注册税移出数据面即达标 |
| Step C footprint 塌缩 | T4b16 / T8b8（inflight 256MB） | 16.6 → **9.8 GB/s**，与连接数/worker 数无关；cpu 同配置 42.5 GB/s 不塌 | **第二墙 = GPU 路径总映射量**（IOTLB 容量假设） |
| 补扫 T2b8 | 64MB footprint | **17.08 GB/s**，无塌缩 | 墙在 128~256MB 之间，无中间梯度 |
| 补扫读方向 | T4b8 `-r` | **12.82 GB/s**（p99 38.3ms vs 写 7.9ms） | 读方向另有 ~23% 独立开销 |
| T8b8 setup 争用 | 8 worker 并发注册 | setup **143s**（~700× 非线性放大） | 注册路径存在串行化点（当时怀疑全局锁） |
| dmesg 逐条打点 | `[UMMU_CORE][MATT]` 日志 | 32MB → 512 条 map+verify，每 64KB 一条 | UMMU 逐 entry map+verify 实锤（来源见第二轮） |

### 2.2 用户态源码核查（SPDK 仓 + GDR 仓 UMDK_netlab）

- **SPDK**：P1 已提交并推送（`8e852647c`，`urma_perf.c:806-830`）；reg cache 128 槽 `(va,len)`
  exact-match（`nvme_urma.c:144/320/334`）；注册入口 4K 对齐逻辑正确；peermem provider
  **无 export_dmabuf**（`urma_perf.c:449-454`，注释明说故意不导出）。
- **UMDK**：hook 7 个拦截点全部 alloc 时一次、无 per-page 工作（`gdr_geforce_hook.c`）
  —— **hook 不在稳态数据路径**；`udma_u_register_seg` 两步链缺一不可（MAPT grant + register
  cmd，`udma_u_segment.c:149`）；**dmabuf 注册全栈未实现**（`urma_seg_dmabuf.c` 是 ENOTSUP
  骨架，SPDK 侧也不导出）→ dmabuf 路线是死路；"supported_alignments 4K/64K/2M/1G"出自
  `urma_gdr_analysis.md` §9 草案注释，**不是代码**。
- 第一轮遗留三问：① verify 是 UMMU 固有还是调试代码？② 注册争用的串行化点在哪？
  ③ UMMU 能否收变长（>64KB）条目？

---

## 3. 第二轮：内核源码核查（GDR 仓 `cf9835d`：urma_driver + kernel_ummu）

### 3.1 三个实锤

| # | 发现 | 位置 | 定性 |
|---|---|---|---|
| 1 | **verify 回读是调试补丁**：每 entry 一次 `iommu_iova_to_phys`（软件页表遍历）+ 比对 + pr_info + mismatch 回滚 | `kernel_ummu/ummu-core/core_matt.c:194-206`（`//TONG ADD FOR DEBUG`，2026/07/21） | 删/参数化即可，**非 UMMU 固有成本** |
| 2 | **每 entry 2 条无条件 pr_info**（map entry + verify）+ bridge get_pages 3-4 条 + GPU-TRACE/UDMA map 各 1 条 | `core_matt.c:180/196`；`udma_nv_p2p_bridge.c:64/154/179/197`；`udma_common.c:279` | printk 走全局 console 锁；T8b8 setup = 8 worker×512 entry×2 ≈ **8192 行 dmesg**，串口上即 143s 量级 → **争用主嫌（Q7 改判**：`global_device_lock` 持锁窗口极小，只是查表） |
| 3 | **bridge 不合并页，UMMU 本来就收变长条目** | `udma_nv_p2p_bridge.c:218-239` 每 64KB 一条 sg；`core_matt.c:160-210` 是 `for_each_sg` + `iommu_map(len=sg_dma_len)` | **coalesce=2m 只改 bridge 一个函数，UMMU 零改动（roadmap 前提 B 通过）** |

### 3.2 实验现象 → 源码机制对照

| 实测现象 | 源码级解释 |
|---|---|
| per-IO 路线 14.6ms/IO（8.5 墙） | 每 4MB IO = 64×（iommu_map + **TLB invalidate** + verify + printk），发生在数据面正在使用同一 IOMMU domain 时 → invalidate 与在飞 DMA 互扰 + printk 全局串行 |
| 整池 7.7ms/IO（16.6） | 512 条一次 setup 完成，数据面期间零 invalidate 零 printk |
| footprint 256MB → 9.8 | 4096×64KB 条目的硬件 TLB 容量压力（翻译表 = 标准 io-pgtable-arm）——**2MB 合并后 256MB = 128 条** |
| unmap 成本（附录 B Q8） | 单次全长 `iommu_unmap`，无逐 entry 回读、无 verify（`core_matt.c:237-275`）→ **unmap 便宜** |
| 读方向 −23% | 与注册路径无关的独立开销（GPU 腿/方向性），仍开放 |

### 3.3 附录 B 问题清单瘦身（发给 UMDK 前）

- **已自答**（源码层面）：Q2（是逐 entry 同步 map+verify，但 verify 是我们的调试补丁）、
  Q7（全局锁窗口极小，争用主嫌 = printk 串行）、Q8（unmap 便宜，非双份税）、
  Q3 一半（UMMU 收变长条目；粒度上限取决于 domain `pgsize_bitmap`，待 node1 一条 grep）。
- **仍需问 UMDK**：Q1（Table mode 实际页粒度/granule）、Q4（hook 路径贵 ~68× 的根因）、
  Q5（有无正式整池/批量注册接口）、Q6（IOTLB 容量、能否大页建表）。

---

## 4. 差距分解（总账）

24.2（链路天花板）− 16.6（当前最优）≈ **7.6 GB/s**，三个病灶：

| 病灶 | 规模/表现 | 状态 | 解法 |
|---|---|---|---|
| ① 注册税（verify 调试 + printk + 逐 64KB 条目） | per-IO 路线争用税；整池路线 setup 内 0.06ms/entry | **两轮定位完成** | 删 verify / 降 pr_debug + coalesce 条目数 ÷32 |
| ② 第二墙（IOTLB/条目数） | inflight 256MB 塌到 9.8 GB/s | 机制为高置信推断，2MB 合并可正向验证 | bridge `coalesce=2m`（大奖） |
| ③ GPU 腿/读方向开销 | read −23%（12.82 vs 16.6） | 独立开放问题 | 注册侧修完后再量，看剩多少 |

---

## 5. 后续解决方案（行动清单，按序）

| # | 行动 | 位置 | 预估 | 判定/产出 |
|---|---|---|---|---|
| 1 | node1 `reset --hard` 对齐 + 重建，复跑整池 T4b8 | node1 | 0.5h | 16.6 从 git 库复现（P1=`8e852647c` 已推送） |
| 2 | **内核 3 行改动**：`core_matt.c` verify 块删除或加模块参数跳过 + map entry/verify 两条 pr_info 降 pr_debug → 重编 ummu → 重跑 per-IO T4b8 | node1 内核 | 1h | 量出 debug 税在 108ms/entry 争用税中的份额；T8b8 setup 143s 若大幅回落即 printk 实锤 |
| 3 | **定粒度**：`dmesg \| grep "map begin"` 看 `pgsize_bitmap`（含 2M → coalesce=2m 可行；仅 {64K,512M} → 改方案）；`GPU-PTE ... lvl=` 佐证已装 PTE 层级 | node1 | 10min | coalesce 方案定稿 |
| 4 | **bridge coalesce=2m（核心赌注）**：`udma_nv_build_phys_sgt` 把物理连续的 32×64KB 合并为 2MB 条目；模块参数 `coalesce=2m` 默认关（仿 `gpu_vendor` 风格）；前提 A（VRAM 连续性）用 GPU-PTE 打点验证，能拼多少拼多少 | node1 `/home/tong/urma_driver` | 0.5~2d | 整池 setup ÷32（512→16 条）；T4b16/T8b8 若 9.8 → 爬升 = 第二墙消失、往 24.2 爬；若不动 = 铁证墙在 UMMU 内部，Q6 带数据外发。**两个结果都有价值** |
| 5 | 复跑矩阵：per-IO T4b8（税）/ 整池 T2b8+T4b8（峰值）/ T4b16+T8b8（墙）/ 读方向 / cpu 注册条目粒度 | node1 | 1h | 全部结果回填本文档 + 附录 B |
| 6 | 修《4090D_V100_peermem.md》乱码/勘误（含 3403 条 map 小账对平）→ 附录 B 瘦身版（Q1/Q4/Q5/Q6）发 UMDK/UB | 文档 | 1h | 外部排期开启 |
| 7 | 硬件兜底：node2 A100 原生 peermem（gen4 + BAR1 64GB），预期 26-28 GB/s | node2 | — | 20+ 目标的保证路径 |

**里程碑**：
- M1（#2 后）：per-IO 稳态 8.5 → ？（debug 税量化，验证 printk/verify 假设）
- M2（#4 后）：整池 setup 0.06ms/entry → ÷32；T8b8 带宽 9.8 → ？（第二墙裁决）
- 终线：**≥ 20 GB/s**。最坏情形（IOTLB 容量写死且不收大页条目）→ 16.6 固化为部署建议
  （inflight ≤128MB），20+ 经 A100 达成，属产品特征而非故障。
