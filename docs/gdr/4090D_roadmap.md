# 4090D GDR 优化路线图（2026-10-08 结题总结 + 下一步计划）

> 定位：本文是 **结论层** —— 我们证明了什么、还剩什么、接下来做什么。
> 证据层（全部实验数据、dmesg、打点）见《4090D_V100_peermem.md》；V100 基线与 gen4 预期见《analysis.md》。
> 环境：SPDK `urma_enable_v2` / UMDK liburma / UB fabric / node1 4090D（GeForce，hook 路线）/ node4 V100（原生路线）。

---

## 0. 三句话结论

1. **4090D GDR 已经从 ~1 MiB/s 崩塌修到 16.6 GB/s**（整池注册 + `-T4 -b8`），超过了 V100 的 13.4 GiB/s（V100 是 gen3 x16 物理墙的 91%）。
2. 剩余差距（16.6 → 目标 20+）**全部定位在 UMDK/UMMU 的 GPU 内存翻译路径**，SPDK 侧已无代码可改；登记税和页表都过我们自己的 `udma_nv_p2p_bridge.ko` 之手，所以有一个**不等 UMDK、自己先动手的大页合并实验**。
3. 硬件兜底是 node2 的 A100（gen4 + BAR1 64GB + 原生 dmabuf，预期 26-28 GB/s）。

比喻版：NVIDIA 驱动是物业（页表给得快），UMDK 的 UMMU 是登记处（逐 64KB 条目建表+核实，登记本疑似只有 2048 条容量），hook.so 是黄牛代办（GeForce 本来不让办这个业务，它开户之后就撤，不在数据路径上）。

---

## 1. 我们证明了什么（事实链，全部有实验支撑）

| # | 结论 | 决定性证据 |
|---|------|-----------|
| 1 | 平台链路健康，gen4 x16 满速 | pinned DtoH = 24.2 GB/s（Step 0） |
| 2 | 注册缓存无罪："4MB 不命中"是**饥饿伪象** | 4K 命中 358 万次/70 miss；`-t 900` wrap 后 4MB 命中 191 万次、稳态 8.5 GB/s（Step 1b/1c） |
| 3 | 慢的根因 = **注册税**：UMMU 逐 64KB entry map+verify | dmesg `[UMMU_CORE][MATT]` 逐条打点；nvidia_p2p 返回 64KB 页（32MB → nents=512） |
| 4 | **verify 本身便宜**（~0.06ms/entry），per-IO 路线的 ~108ms/entry 是**争用税** | 整池 setup 203ms 窗口内 3403 条 map+verify 全部同秒完成（① 决定性检查）；成本模型修正为"上下文相关" |
| 5 | 8.5 GB/s 墙 = **per-IO seg 处理开销墙**，不是 hook 映射墙 | 整池注册后同参数 16.6 GB/s、窗口内 0 注册（Step 1e） |
| 6 | **hook 不在稳态数据路径**，只在使能层 | 4 个拦截点全在 alloc 时（attr116/cuMemCreate/ioctl REGISTER_VIDMEM/SYNC_MEMOPS）；数据面打点 µs 级 |
| 7 | 第二墙 = **GPU 路径总映射量**（footprint 256MB 塌缩到 9.8） | T8b8 与 T4b16 同塌（与连接数/worker 数无关）；cpu 同配置 42.5 GB/s 不塌（Step C ②） |
| 8 | V100 的 90% 之谜：gen3 链路（raw 15.75）**低于** UMMU 墙（~16.6），链路先饱和 | V100 在 512MB inflight 有同形塌缩（13.4→9.4）→ 墙是跨卡共有的 UMMU 特性；同 footprint 下 4090D 已更快（16.6 vs 13.4） |
| 9 | cpu 47 GB/s = **host 路线的 fabric 天花板**，非系统统一上限 | cpu 与 GPU 过同一套 UMMU，但 host 大页条目少、DMA 直读 RAM 无 GPU 腿；per-IO 延迟 cpu 3.0ms vs GPU 7.7ms（同 T4b8） |

## 2. 尚未证明 / 待定（引用时别说成"已证明"）

1. **IOTLB 容量假设**："2048/4096 entries 边界"是与全部观测自洽的**高置信推断**，非直接实测——IOTLB 真实大小只有 UMDK 能确认（附录 B Q6）。
2. **host 注册的条目粒度**："cpu 大页 → 条目少 32 倍"是推断，没在 dmesg 里数过 host 注册的 map entry 粒度（补证方法见 §3.3-2）。
3. **hook 路径注册每页贵 ~68× 的根因**：是伪句柄逼出 64KB 粒度，还是 UMMU 对该路径区别对待——未定（附录 B Q4）。
4. **16.6 是否峰值**：是实测最高点；周边配置（64MB footprint、1-2MB IO、读方向）未扫完。

---

## 3. 优化方案（分层）

### 3.1 已落地（SPDK 侧，收工）

**整池注册 P1**（`urma_perf.c:810-824`，15 行）：`URMA_PERF_REGION_REG=1` 时 gate 放行 peermem，type 按 mem_type 传 `SPDK_NVME_URMA_MEM_CUDA`。

- 效果：首圈注册税移出计时窗（T4b8 setup 仅 ~0.2s）+ 稳态 8.5 → **16.6 GB/s**（顺带绕过 per-IO seg 开销墙）。
- ⚠️ **该改动目前只存在于 node1 工作区，未进任何 git——下一次 `reset --hard` 对齐就会把它抹掉。抢救进 git 是全部行动计划里最优先的一项（§5 行动 1）。**

### 3.2 部署达标配置（今天就能交付的答案）

```
GPU peermem：URMA_PERF_REGION_REG=1  -o 4M -T 4 -b 8   → 16.6 GB/s（inflight 128MB）
```

- inflight 勿超 128MB：256MB 触发第二墙（9.8 GB/s），属 UMDK 范畴，部署上直接避开。
- posix（staged，5.4 GB/s）与 cpu（38 GB/s）不受影响，照常。

### 3.3 自主实验（不等 UMDK，按性价比排序）

**1. bridge 层大页合并 —— 本方案的核心赌注**

原理：`nvidia.ko` 没有"给大页"的开关（页粒度由 RM 决定，恒吐 64KB 条目），但页表在返回 UMMU 之前**过我们自己的 `udma_nv_p2p_bridge.ko`**（源码在 node1 `/home/tong/urma_driver/`）。在 bridge 里把**物理连续**的 32 条 64KB 合并成 1 条 2MB 再交 UMMU——UMDU 自己声明 `supported_alignments: 4K/64K/2M/1G`。

- 前提 A（10 分钟，只读）：**VRAM 物理连续性** —— 打印 64KB 物理地址，数 512 条里有几个连续 2MB 段。新分配的大池子（GPU 刚重启、无碎片）大概率几乎全连续；做法上"能拼多少拼多少"，不假设全连续。
- 前提 B（10 分钟，只读）：`ummu_sva_matt_map_phys_sg` 入口是否收**变长条目**（标准 sg 每条自带长度 → 直接喂；固定 page_size → 连 page_size 参数一起改 2MB）。
- 实现：挂模块参数（如 `coalesce=2m`，与现有 `gpu_vendor` 参数风格一致，默认关，不影响默认路径）。
- 预期收益一：**注册税 ÷32**——整池 setup 从 0.06ms/entry×512 条 → 16 条；T4b16/T8b8 那种 setup 126~143s 的场景缩到几秒。
- 预期收益二（大奖）：若数据面 IOTLB 按**条数**计费，256MB 从 4096 条降回 128 条（与 cpu 大页同级），**第二墙消失，带宽往 ~24 的链路天花板爬**。
- 若落空（UMMU 内部把 2MB 映射拆回 64KB IOTLB 条目）：墙不动，但拿到铁证"墙在 UMMU 内部"，附录 B Q6 带数据发 UMDK。**两个结果都有价值。**

**2. 便宜补扫（各 60s）**

| 实验 | 回答什么 |
|------|---------|
| 整池 T2b8（64MB footprint） | 若 >16.6 → 128MB 已在墙的斜坡上；兼出更优部署配置 |
| 读方向 `-r` A/B | 写方向结论的对称性验证 |
| cpu 整池注册时数 dmesg `map entry` 粒度 | 验证"host=2MB 条目"推断（32MB 出 16 条还是 512 条），结果直接进附录 B |
| T4b8 整池 setup 的 dmesg 小账 | 3403 条 map vs 预期条数对不上的 ~1100 条差额来源（grep 窗口残留 / preflight 是否额外注册） |

### 3.4 UMDK 侧（外部依赖）

把《4090D_V100_peermem.md》附录 B 的 8 问发给 UMDK/UB 团队：

| # | 问题 |
|---|------|
| Q1 | UMMU Table mode 支持的页粒度？64KB/2MB 大页是否可用（声明里有，实测没用上）？ |
| Q2 | 注册是否每 entry 同步 map+verify？能否批量映射并跳过/抽查 verify 回读？ |
| Q3 | 能否直接按 64KB/2MB 粒度处理 nvidia_p2p 页表（4MB → 64 项而非 1024 项）？ |
| Q4 | hook/GeForce 路径注册贵 ~68× 的根因在 UMMU 侧还是 hook 侧？ |
| Q5 | 有无正式的整池/批量注册接口（我们已用 SPDK 侧整池验证可行，16.6 GB/s）？ |
| Q6 | **第二墙**：footprint 128→256MB 带宽 16.6→9.8 塌缩（与连接数无关、cpu 不塌）——是否 IOTLB 容量上限？IOTLB 多大？能否大页建表？ |
| Q7 | **setup 注册争用**：8 并发注册 700× 非线性放大（25ms → ~18s/条），有无全局锁/串行化？ |
| Q8 | **unmap 成本**：per-IO 注册配对的 unmap 是否也逐 entry 回读？若是，首圈税实际是双份。 |

发出前先把《4090D_V100_peermem.md》修到可转发状态（§4）。

### 3.5 硬件兜底

node2 A100：gen4 x16 + **BAR1 64GB** + 原生 dmabuf 可测，analysis.md 预期 26-28 GB/s，不受 GeForce hook/BAR1(256MB) 限制。若 UMDK 短期不动而 20+ 必须达成 → A100 是保证路径。

---

## 4. 对《4090D_V100_peermem.md》的审阅待修项

实质（3 处）：

1. **L233（§3.5 Step 1d "嫌疑排序裁决"）**：旧终判"① hook 伪 P2P 映射带宽墙 → 实锤"已被 Step 1e 证伪（hook 不在数据路径）——加划线勘误指向 Step 1e，与 VLLM 勘误同格式。
2. **"IOTLB 容量故事坐实"措辞**（§0 TL;DR 与 §3.5 Step C）：改"高置信推断"——Q6 里问 IOTLB 大小本身就承认了未实测。
3. **3403 条 map 的小账**（§3.5 Step 1e）：与 n=8 次注册的预期条数对不上（32MB 整池应为 512 条/次），差 ~1100 条——确认 grep 窗口无残留、preflight 是否额外注册，附录 B 发出前对平。

文字硬伤（乱码/截断，附录 B 发出前必修，按行号）：

| 位置 | 问题 |
|------|------|
| L350 / L358 / L372 / L423 / L435 | 乱码（`重æ¨册`、`äep`、`当åm`、`ä化`、`数æ§§`） |
| L366 / L370 / L373 / L382 / L387 / L392 / L411 / L420 / L425 | 句子/表格截断 |
| L400 | `#2 ——` 应为 `## 附录 B：待问 UMDK/UB 团队的问题清单` |
| L407 | 附录 B 数据表表头与分隔行断裂 |

---

## 5. 下一步行动清单（按优先级）

| # | 行动 | 位置 | 预估 | 产出 |
|---|------|------|------|------|
| 1 | **抢救 P1 15 行改动进 git**：从 node1 `urma_perf.c:810-824` 取 diff → 本地 commit → **立即 push** → node1 `reset --hard` 对齐重建 → 复跑 T4b8 确认 16.6 从库里复现 | node1 + 本仓库 | 30 分钟 | 改动不再悬空，部署链顺带验证 |
| 2 | 修 §4 审阅待修项（勘误 + 乱码） | 《4090D_V100_peermem.md》 | 30 分钟 | 附录 B 可转发 |
| 3 | 附录 B 发 UMDK/UB 团队 | 外部 | — | 开启 UMDK 侧排期 |
| 4 | bridge 大页合并：前提 A/B 验证 → `coalesce=2m` 实现 → setup 验证 ÷32 → T4b16/T8b8 看第二墙动不动 | node1 `/home/tong/urma_driver` | 0.5~2 天 | 正反结果都直接进附录 B Q6 |
| 5 | 便宜补扫：T2b8 整池 / 读方向 / cpu 注册条目粒度 | node1 | 1 小时 | 峰值确认 + 推断补证 |

## 6. 目标与判定

- **总目标**：4090D GDR ≥ 20 GB/s（对齐 analysis.md 的 gen4 预期 24-28 GB/s 下沿）。
- **当前**：16.6 GB/s = gen4 raw（31.5）的 53%，已超 V100 的 13.4（gen3 硬件墙 91%）。
- **路径**：bridge 大页合并（自主、先赌）→ UMDK 修 IOTLB/大页建表（外部、正解）→ A100（硬件兜底）。
- **最坏情形**：UMMU IOTLB 容量写死且不收大页条目 → 4090D 单卡钳在 ~16.6（`-T4 -b8` 固化为部署建议）。这属于产品特征而非故障；20+ 目标经 A100 达成，不受此限。
