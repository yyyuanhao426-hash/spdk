# 4090D peermem 源码核查报告（GDR 仓 + SPDK 仓对照，2026-10-10）

> 定位：**证据层补丁** —— 把《4090D_V100_peermem.md》的运行时诊断（dmesg/打点）钉到具体源码行，
> 并记录两个此前文档没有的硬发现（dmabuf 全栈死路、supported_alignments 是草案不是实现）。
> 核查对象：
> - https://github.com/LeeJace993/GDR（本地 `/home/senorita/GDR`，单 commit main：UMDK_netlab 源码快照 + kernel 6.6.0 config + iommu patch）
> - `/home/senorita/nof/spdk`（urma_enable_v2，本地与 origin 完全同步）

---

## 0. 一句话结论

文档诊断链（饥饿伪象 → per-page 注册税 → 整池注册绕过 → footprint 第二墙）**在源码层面全部对得上**；
P1 已抢救进 git（行动 1 前半段完成）；新增两个修正：**dmabuf 路线在本栈是死路（双端都未实现）**、
**"UMMU 声明支持 2M/1G"出自设计草案注释而非代码**——附录 B 发出前按 §4 改措辞。

---

## 1. P1（整池注册放行 peermem）git 状态：已入库、已推送

- 提交：`8e852647c` "feat(urma_perf): 整池注册放行 peermem——URMA_PERF_REGION_REG=1 时 gate 覆盖 peermem，
  type 按 mem_type 传 SPDK_NVME_URMA_MEM_CUDA…（Step B）"，位于 `urma_perf.c:806-830`。
- `git log origin/urma_enable_v2..urma_enable_v2` 为空 → **本地与 origin 同步，已推送**。
- 路线图 §5 行动 1 的前半段（抢救进 git）✅ 完成；剩余后半段：node1 `reset --hard` 对齐重建后
  复跑 `-o 4M -T 4 -b 8` 确认 16.6 GB/s 从库里复现（《4090D_roadmap.md》§3.1 的"未进任何 git"警告已过时，可删）。

---

## 2. 注册链源码级证据（把 dmesg 证据钉到具体函数）

### 2.1 SPDK 侧（urma_enable_v2）

| 环节 | 位置 | 事实 |
|---|---|---|
| 64KB 对齐分配 | `urma_perf.c:24` `URMA_PERF_GPU_ALIGNMENT (64*1024)` | nvidia_p2p 要求 64KB 对齐 va/len |
| peermem provider | `urma_perf.c:449-454` `g_cuda_provider_peermem` | **只有 .pin/.unpin，无 export_dmabuf**（注释明说故意不导出） |
| provider pin | `urma_perf.c:415-434` `cuda_provider_pin` | 纯记账（alloc 表 + offset），**不真 pin**——真 pin 在内核 gpu pin |
| 注册入口 | `nvme_urma_common.c:345` `spdk_nvme_urma_register_memory` | provider->pin → **base 下对齐 4K、len 上取整 4K** → `urma_register_seg(cfg)`，`cfg.is_gpu_seg = (type != HOST)` |
| UMMU 页粒度注释 | `nvme_urma_common.c:396`（Modified by Yin） | "grant 长度向上取整到 4K，**UMMU Table mode 页粒度**"；v3 注释：Table mode 要求 4K 对齐 base 覆盖完整页，wire capsule 仍携带原始 IO 地址 |
| per-IO 注册缓存 | `nvme_urma.c:144/320/334` | 128 槽 `(va,len)` exact-match，lookup/insert 同 key——与文档 §"源码证据链"一致 |

### 2.2 UMDK 侧（liburma，GDR 仓）

`udma_u_segment.c:149 udma_u_register_seg` = 两步，缺一不可（注释原文：*"MAPT grant is still required
for GPU segs (token/perm). Kernel gpu pin only replaces pin_user_pages; it does not install MAPT."*）：

1. **`udma_u_grant_segment` → `ummu_grant(tid, va, len, perm)`** —— MAPT 建表。
   内核侧 `ummu_sva_matt_map_phys_sg` 逐 64KB entry map+verify（dmesg `[UMMU_CORE][MATT]` 实锤）。
   **注册税就在这一步**：per-IO 路线每 4MB 调一次 grant（64 entries×争用税），整池路线每 32MB 调一次
   （512 entries 无争用 0.06ms/entry）——与《4090D_V100_peermem.md》§4.1a 成本模型吻合。
2. **`udma_exec_register_seg_cmd` → `urma_cmd_register_seg` + `ucmd{is_gpu_seg=1, reserved=0}`**
   （`udma_u_segment.c:21-24`，ABI 必须与内核 `udma_register_seg_ucmd` 一致）→ 内核 udma.ko gpu pin
   → `udma_nv_p2p_bridge.ko` → `nvidia_p2p_get_pages`（64KB 页）。

- **ungrant 配对**：`udma_u_ungrant_seg` → `ummu_ungrant(tid, va, len)` 全长调用（注册失败/注销都会走）。
  内核侧成本仍未单独测（附录 B Q8 维持）。
- **UMMU 库不在本仓**：`hw/udma/CMakeLists.txt:25 target_link_libraries(... ummu)`——libummu 是外部库，
  UMMU 内部（IOTLB 大小、锁、页粒度）在 GDR 仓/UMDK 仓都看不到，只能问 UMDK（附录 B）或在 node1
  内核源（`/home/tong/urma_driver`）里查。
- ABI 扩展位：`urma_types.h:999` `is_gpu_seg`（注释：1 = GPU VA; kernel pins via vendor P2P），
  后续 dmabuf 字段的扩展位置已在 1000-1003 行注释预留（**未实现**）。

### 2.3 hook 源码核对（证实《4090D_V100_peermem.md》§4.1b）

`userspace/gdr-geforce-hook/gdr_geforce_hook.c`（1130 行）实际导出 7 个拦截点：
`ioctl(:540)` / `cuDeviceGetAttribute(:910)` / `cuMemAlloc_v2(:928)` / `cuMemAlloc(:943)` /
`cuPointerSetAttribute(:949)` / `cuMemCreate(:977)` / `cuMemMap(:1007)` / `dlsym(:1104)`。
全部工作在 **RM ioctl（major 195，/dev/nvidia*）的 RM_ALLOC/RM_CONTROL** 上：
NV50_THIRD_PARTY_P2P(0x503c) + REGISTER_VA_SPACE(0x503c0102) + REGISTER_VIDMEM(0x503c0104)，
**alloc 时一次、无 per-page 工作** → "hook 只在使能层、不在稳态数据路径"结论成立。

内核侧 `kernel-6.6.0-netlab_pcie_ub_compat/patches/0001-iommu-gpu-pte-debug.patch`：
`arm_lpae_init_pte` 对 0x8000_xxxx_xxxx 段打 GPU-PTE 打点（iova/paddr/raw/attrs/lvl/nents）+
把 `iommu_map_sg` 里逐条 sg_phys 噪音注释掉——这正是路线图 §3.3-1 **前提 A（VRAM 物理连续性）**
的观测工具，工具已备。

---

## 3. 新发现 A：dmabuf 路线全栈死路（影响附录 B Q5、analysis.md A100 建议）

两端独立核实，dmabuf 注册在本栈**没有一寸真实现**：

| 层 | 位置 | 事实 |
|---|---|---|
| SPDK | `urma_perf.c:449-454` | peermem provider 无 `export_dmabuf` 字段 → `nvme_urma_common.c:400` 的 dmabuf 分支直接跳过（注释明说故意） |
| SPDK | `nvme_urma_common.c:418-423` | 即便 dmabuf 尝试成功，`target_seg == NULL` 后照样落回 `urma_register_seg(is_gpu_seg=1)` 同一条 peermem 路径 |
| UMDK | `urma_seg_dmabuf.c:65-110` | `urma_register_seg_dmabuf` 是 **comment-only 骨架**：校验完参数一律 `errno = EOPNOTSUPP; return NULL;`（"不访问 ctx->ops->register_seg_dmabuf（字段尚未加入 ABI），不发任何 IOCTL"） |
| UMDK | `urma_seg_dmabuf.c:118-149` | `urma_query_dmabuf_caps` 返回**全 0**（support_dmabuf=0、supported_alignments=0），"driver feature bit not wired" |

**含义**：
1. 附录 B Q5 答案先自答一半：UMDK 当前**没有**正式的整池/批量注册接口，dma-buf 注册接口只有骨架
   （等 urma_ops ABI 落地后解开注释才是真实现，见 urma_seg_dmabuf.c 头注释）。
2. 《analysis.md》§5.2 #6 / §7 / §8.3 的"换 A100/H100 可测 `-M dmabuf`"**不成立**：dmabuf 导出即使
   在 A100 上成功，注册仍会掉进 is_gpu_seg=1 的 nvidia_p2p 路径。A100 的收益只有原生 peermem
   （gen4 + 大 BAR1 + 正常注册税），dmabuf 要等 UMDK 把 ABI 真正接上。
3. 也解释了为什么 4090D 上 `dmabuf=0` 全走 `peer_memory_fallback`：不是"导出失败回退"，
   而是"根本没有第二条路"。

---

## 4. 新发现 B："supported_alignments: 4K/64K/2M/1G" 是设计草案注释，不是 UMMU 声明

- **出处**：《GDR 仓 `UMDK_netlab/docs/urma_gdr_analysis.md`》§9 附录"拟新增的用户态头草案"里的
  注释行 `/* bitmap: 4K/64K/2M/1G */`——**草案示意**，不是 UMDK/UMMU 代码。
- **代码现状**：`urma_query_dmabuf_caps` 是 stub（supported_alignments 恒 0）；整个 UMDK 源码
  `grep supported_alignments` 只命中注释；UMMU 内核驱动不在本仓，无从证实 2M/1G 支持。
- **需要修正的引用**（两份文档）：
  - 《4090D_V100_peermem.md》§4.1a-3 与 §5-P2："urma_gdr_analysis.md 显示 UMMU
    `supported_alignments: 4K/64K/2M/1G` 表明 UMMU 已声明支持 2M/1G" →
    改为"设计草案曾提出 4K/64K/2M/1G 位图（urma_gdr_analysis.md §9 草案头），代码未实现，
    UMMU 真实页粒度待 UMDK 确认"。
  - 《4090D_roadmap.md》§3.3-1："UMDU 自己声明 supported_alignments: 4K/64K/2M/1G" →
    同上。**bridge 大页合并实验的价值不变**（前提 B"ummu_sva_matt_map_phys_sg 是否收变长条目"
    本来就计划在 node1 只读实测），但"UMMU 自己声明过支持 2M/1G"这句不能再当依据引用。
- **附录 B Q1/Q3 措辞**：从"你们声明了支持为什么没用上"改为"请确认 UMMU Table mode 实际支持的
  页粒度（4K？64KB？2MB？）"——问题的本质没变，但证据等级要诚实。
- SPDK 侧唯一实锤的页粒度线索是 `nvme_urma_common.c:396` 注释："**UMMU Table mode 页粒度** = 4K"
  （grant len ceil 到 4K）——与 dmesg 观察到的"nvidia_p2p 吐 64KB entry、UMMU 逐 entry map+verify"
  合起来：Table mode 条目粒度 4K 起，实际条目大小由 pin 路径给的 phys-sg 决定（当前恒 64KB）。

---

## 5. 结论与剩余行动（对照《4090D_roadmap.md》§5）

| # | 行动 | 状态 |
|---|------|------|
| 1 | 抢救 P1 进 git | ✅ **已完成**（`8e852647c` 已提交并推送）；剩 node1 对齐重建 + T4b8 复跑确认 |
| 2 | 修《4090D_V100_peermem.md》乱码/截断/勘误 | 待做；**本次新增 2 处必修**：§4.1a/§5-P2 的 supported_alignments 表述（见 §4）、乱码清单照旧 |
| 3 | 附录 B 发 UMDK/UB | 依赖 #2；Q5 建议按本报告 §3 自答一半（无整池接口、dmabuf 仅骨架），Q1/Q3 措辞按 §4 修正 |
| 4 | bridge 大页合并（node1） | 前提 A 的观测工具（iommu PTE debug patch）已在 GDR 仓；前提 B 照计划在 node1 验证 |
| 5 | 便宜补扫（T2b8/读方向/cpu 条目粒度） | 待做（node1） |

**附**：analysis.md 需要连带修正的一处——《analysis.md》§5.2 #6、§7、§8.3 中"换 A100 可测
`-M dmabuf`"的说法（本报告 §3-2），A100 的正确预期是原生 nvidia_p2p peermem 路线。

---

## 6. 内核源码核查（GDR 仓 cf9835d：urma_driver + kernel_ummu，2026-10-10）

> 拉取 `cf9835d`（urma_driver 内核源 + kernel_ummu/HiSilicon UMMU 实现）后完成的静态核查。
> **核心结论：注册税的三个成分全部找到，其中两个是调试代码，一个是 bridge 未合并。**

### 6.1 注册链全景（dmesg 证据逐条钉死）

```
ioctl REGISTER_SEG(is_gpu_seg=1)
→ udma_pin_pages_and_ioummu_map (udma_segment.c:593)
   ├─ udma_gpu_pin_seg_pages (udma_gpu_p2p.c:283)
   │    → bridge pin: nvidia_p2p_get_pages（恒 64KB 页）→ udma_nv_build_phys_sgt
   │      （udma_nv_p2p_bridge.c:218-239，每 64KB 一条 sg，4MB=64条、32MB=512条，无合并）
   │    + pr_info "[GPU-TRACE] pin OK" + get_pages 3-4 条 pr_info
   └─ udma_seg_ioummu_map（udma_segment.c:480-567，全程持 ctx->seg_tree->lock）
      → udma_ioummu_map_phys_sg（udma_common.c:266，+1 条 pr_info）
        → ummu_sva_matt_map_phys_sg（core_matt.c:417）
          → for_each_sg 逐条（ummu-core/core_matt.c:160-210）:
             pr_info("[PHYS-SG] map entry")  ← 每 entry 一条，无条件
             iommu_map(domain, iova, pa, 64KB)  ← arm_lpae 装 PTE + TLB invalidate
             iommu_iova_to_phys + 比对           ← 【TONG DEBUG 补丁】
             pr_info("[PHYS-SG] verify")      ← 每 entry 一条，无条件
```

### 6.2 三个实锤

| # | 发现 | 位置 | 性质 |
|---|------|------|------|
| 1 | **verify 回读是调试补丁**：每 entry 一次 `iommu_iova_to_phys`（软件页表遍历）+ pr_info + mismatch 回滚 | `core_matt.c:194-206`（`//TONG ADD FOR DEBUG`，2026/07/21） | **调试代码，非 UMMU 固有成本**。dmesg 全部 verify 行来自它 |
| 2 | **每 entry 2 条无条件 pr_info**（map entry + verify）+ bridge get_pages 3-4 条 + "UDMA GPU map" 1 条 | `core_matt.c:180/196`、`udma_nv_p2p_bridge.c:64/154/179/197`、`udma_common.c:279` | printk 走全局 console 锁；T8b8 整池 setup = 8 worker×512 entries×2 ≈ **8192 行 dmesg** —— setup 143s（700× 争用税）的主要嫌疑（串口 console 每行可达 ~10ms） |
| 3 | **bridge 不合并页 + UMMU 本来就收变长条目** | bridge `udma_nv_build_phys_sgt` 每 64KB 一条 sg；`sva_matt_inner_map_phys_sg` 是 `for_each_sg` + `iommu_map(len=sg_dma_len)` | **路线图前提 B 通过**：`coalesce=2m` 只需改 bridge 一个函数，UMMU 侧零改动 |

### 6.3 机制全部对上（每个实测现象有了源码级解释）

- **per-IO 路线 14.6ms/IO（8.5 墙）**：每 4MB IO 触发 64×（iommu_map+TLB invalidate+verify+printk），
  发生在数据面正在使用同一 IOMMU domain（`iommu_sva_bind_device_isolated` 的专用 domain，
  `udma_ctx.c:116`）时 → TLB 失效与在飞 DMA 互相干扰 + printk 全局串行。
- **整池 7.7ms/IO（16.6）**：512 条一次 setup 完成，数据面期间零 invalidate 零 printk。
- **footprint 第二墙（256MB→9.8）**：硬件 TLB 对 4096×64KB 条目的容量压力（翻译表 = 标准
  io-pgtable-arm，内核 patch 正打在 `arm_lpae_init_pte`）——**2MB 合并后 256MB = 128 条**，
  与 cpu 大页同级，墙有望消失。
- **Q8（unmap）已答**：GPU unmap = `udma_ioummu_unmap_phys_sg` → 单次全长 `iommu_unmap`，
  **无逐 entry 回读、无 verify**（`core_matt.c:237-275`）→ unmap 便宜，首圈税不是双份。
- **Q7（全局锁）修正**：`global_device_lock` 存在但持锁窗口极小（get_tid_mm/get_sva_separated_dev
  查表）；setup 争用的主嫌从"全局锁"改为 **printk console 串行化**（实锤 2）。

### 6.4 待 node1 一条 grep 定夺：2MB 块 PTE 可行性

`[PHYS-SG] map begin` 打印 `pgsize_bitmap=0x%lx`（`core_matt.c:158`）——决定 coalesce 粒度上限：
- 4K granule（bitmap 含 2M/1G）→ `iommu_map(2MB)` 出 1 条 lvl2 block PTE，coalesce=2m 满收益；
- 64K granule（bitmap = {64K,512M}）→ **无 2M 档**，需按 64K 拼更大块或直接 512M（不现实）。
另：内核 patch 的 `GPU-PTE ... lvl=` 字段直接显示已装 PTE 层级（node1 dmesg 可 grep）。

### 6.5 行动清单（内核侧，按序）

1. **3 行改动先做（零风险，量化 debug 税）**：`core_matt.c` verify 块**注释保留（勿删——他人可能需要）**或加模块参数默认跳过 +
   两条 pr_info 降为 pr_debug（同样注释/降级，勿删）→ 重编 kernel_ummu → 重跑 per-IO T4b8 → 直接量出 printk+verify
   在 108ms/entry 争用税里的份额。
2. `dmesg | grep "map begin"` 看 pgsize_bitmap → 定 coalesce 粒度。
3. bridge `coalesce` 模块参数（与 `gpu_vendor` 风格一致，默认关）：`udma_nv_build_phys_sgt`
   里把物理连续的 32×64KB 合并为 1 条 2MB sg（前提 A 连续性可用内核 patch 的 GPU-PTE 打点验证）。
4. 重跑 T4b16/T8b8 整池 → 看 footprint 第二墙动不动（正反结果都进附录 B Q6）。

---

## 7. 第三轮：cf9835d 全量逐行核对 + 两个悬念定案（2026-10-11）

> 对 §6 的全部论断在真实源码上逐行复核，并把 §6.4 "待 node1 实测"的两个问题从源码侧定案。
> 结论先行：**§6.2 三个实锤全部属实**（行号逐一对上）；**GPU 映射 domain 的 pgsize_bitmap =
> 硬件 cap 定案后，coalesce=2m 可行性只剩硬件 GRAN 位一个问题，node1 上 1 分钟可查**。

### 7.1 §6 实锤复核结果

| 论断 | 复核 | 结论 |
|---|---|---|
| verify 回读调试补丁 | `core_matt.c:194-206` `//TONG ADD FOR DEBUG`，每 entry `iommu_iova_to_phys` + pr_info + mismatch 回滚 | ✅ 属实。处置（2026-10-11 拍板）：**注释保留勿删**（他人可能需要），或加模块参数默认跳过 |
| 每 entry 2 条 pr_info | `core_matt.c:180/196` + map begin(`:155`)/success(`:214`) + unmap 2 条(`:256/:267`) + `udma_common.c:280/303`（UDMA GPU map/unmap）+ bridge 10 条 pr_info/pr_warn | ✅ 属实（printk 量比 §6 估算略多；处置同样注释/降 pr_debug，勿删） |
| bridge 不合并、UMMU 收变长条目 | `udma_nv_p2p_bridge.c:218-239` 硬编码 `NVIDIA_P2P_PAGE_SIZE_64KB` 逐条 sg；`core_matt.c:160-210` 纯 `for_each_sg` + `iommu_map(len)` | ✅ coalesce 只改 bridge 成立 |
| Q7 改判 printk（全局锁窗口小） | `global_device_lock` 只包查表（`core_matt.c:18-34`）；真正的大持锁是 **per-ctx** `seg_tree->lock` 跨 512 条 map 全程（`udma_segment.c:486-564`），不跨 worker | ✅ 一致，串行化主嫌仍是 printk |
| Q8 unmap 便宜 | `core_matt.c:237-275` 单次全长 `iommu_unmap`，无 verify（但 3 条 pr_info） | ✅ 属实 |

### 7.2 定案一：node1 = separated-tdev 模式，GPU 映射的 domain bitmap = 硬件 cap

- config 第 6792 行 `CONFIG_UB_UMMU_SVA_SEPARATED_PAGES=y`；`core_matt.c` 仅在该开关下编译
  （ummu-core/Makefile:12），node1 dmesg 有 `[UMMU_CORE][MATT]` → **separated-tdev 模式在用**
  （`udma_ctx.c:88-94` 走 `ummu_core_alloc_separate_tdev`，GPU 路径 `udma_segment.c:519` →
  `udma_ioummu_map_phys_sg` → `ummu_sva_matt_map_phys_sg`）。
- GPU 映射落进 **tdev 平台设备的默认 DMA domain**（`core_tdev.c:193-211` fwspec 挂 UMMU；
  `core_matt.c:146` `iommu_get_domain_for_dev`），bitmap 来自**硬件 cap**：
  `ummu_main.c:278-292`——GRAN4K→`{4K,2M,1G}`、GRAN64K→`{64K,512M}`、GRAN16K→`{16K,32M}`
  （`page_table.c:58/148`）。
- **关键澄清（差点误判）**：`sva.c:474` 把 SVA domain 的 `pgsize_bitmap` 强制成 `PAGE_SIZE`——
  但只作用于 share 模式的 pasid 级 SVA domain（且 SVA domain ops 无 `map_pages`，`sva.c:445-455`，
  映射实证走的是 default domain），**不影响 GPU 路径，不是 coalesce 的死刑**。
- → **2M block-PTE 可行性只剩硬件 GRAN 位一个问题**，node1 上 1 分钟定案：
  `grep "map begin" 旧dmesg`（§6.4 计划的那条）或 `cat /sys/.../pgsize_bitmap`（`attribute.c:56` 现成 sysfs）。
  判读：`0x40201000`={4K,2M,1G} → coalesce=2m 满收益；仅 `0x20010000`={64K,512M} → 无 2M 档需改方案。

### 7.3 定案二：PTE 数学修正（第二墙比 §6.3 建模的更严重）

4K granule 下 domain bitmap={4K,2M,1G} **不含 64K**：现在每条 64KB sg 经 `iommu_map` 被拆成
**16 条 4K PTE**（`iommu_pgsize` 从 domain bitmap 选最大可整除档）。即：

- 256MB footprint = **65536 个 4K TLB 条目**，不是 §6.3 的"4096×64KB 条目"——TLB 压力被低估 16×，
  第二墙反而更说得通；
- coalesce=2m 的 TLB 收益 = **512× 缩减**（65536→128），不是 32×——赌注价值比原计划更大；
- （若硬件仅 GRAN64K：现状确为每条 1×64K PTE，§6.3 数字成立，但 2M 档不存在，方案需改。）

### 7.4 新发现

1. **`core_matt.c:152-153` 强制 `IOMMU_MMIO`——属刻意设计（2026-10-11 项目侧确认，非嫌疑）**：
   `iommu_prot = prot & ~IOMMU_CACHE; iommu_prot |= IOMMU_MMIO;`
   ——GDR 翻译的对象是 **GPU BAR 空间地址**，BAR/设备内存不能按 cacheable 属性建表
   （`IOMMU_CACHE`→Normal WB 会破坏翻译/访问），剥离 CACHE、强制 Device-nGnRE 是**正确性要求**。
   原先"③读方向 −23% 的属性带宽嫌疑"相应降级：仅在①②修完、③仍无解释时，且确认 BAR 窗口
   读无副作用后，才可试 Normal-NC（**绝不可回 CACHE**）。
2. **cpu/GPU 不对称性的机制实锤**：cpu 路径 `sva_matt_inner_map` → `iommu_map_sgtable`
   （`core_matt.c:86`，逐 entry 按 domain bitmap 选最优 pgsize，大页直接 2M block）vs GPU 路径
   逐条固定 64KB `iommu_map`——"cpu 不塌、GPU 塌"有了源码级机制对应。
3. 小项：GPU 路径开关 `CONFIG_UMMU_MATT_PHYS_SG` 是 `udma_common.h:22` **硬编码 `#define 1`**
   （非 Kconfig），改内核无需动编译配置；coalesce=2m 需 iova 2M 对齐（CUDA VMM 2MB 粒度满足；
   per-IO 64K 对齐场景退化，bridge "能拼多少拼多少"正好兜住）。

### 7.5 对行动清单的修订

| 行动 | 修订 |
|---|---|
| §6.5-1 删 verify/降 printk | verify 块**注释保留（勿删）**或模块参数默认跳过；pr_info 同样注释/降 pr_debug；其余不变，仍是第一步零风险项 |
| §6.5-2 定粒度 | 从"10 分钟"降为 **1 分钟**：旧 dmesg capture `grep "map begin"` 或 sysfs 直读，无需跑测试 |
| §6.5-3 coalesce=2m | 不变；预期收益改为"TLB 条目 512×（65536→128）" |
| 新增（已降级为可选） | `IOMMU_MMIO`→Normal-NC A/B：经项目侧确认 MMIO 属刻意设计（翻译对象是 GPU BAR 空间，不能 cacheable）——仅当①②修完③仍在才试，且**不可回 CACHE**（§7.4-1） |
| 附录 B Q6 | 保持；可带上新证据——kernel_ummu **无软件 IOTLB**（flush.c 全是硬件 TLBI 命令），容量只有 UMDK/硬件文档能答；粒度问题可换算成 4K/2M PTE 层级表述 |
