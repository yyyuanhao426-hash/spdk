# Bonding capsule 池的页隔离修复

2026-11-26，`urma_enable_v3`。对应 Doc 65 / Doc 66 的 target 注册
`Address range overlap -> register_failed -> rx_init` 指纹。

## 结论与范围

bonding 将一次逻辑注册展开为成员 pseg 和聚合 vseg，本地 UMDK 的
`src/urma/lib/urma/bond/bondp_segment.c` 中 `bondp_create_pseg()` /
`bondp_create_vseg()` 可确认该机制。它不意味着 SPDK 把 recall 的某个缓冲
错误地直接注册了两遍，也不能据此把失败概率写成固定 N+1 倍。

本次在 SPDK 侧消除一个确定的冲突来源：不同 capsule 池虽然是独立的 `calloc`
对象，但首尾可能共享一个 4 KiB 页；注册层将每个对象分别向下/向上扩展到完整
4 KiB 范围，两个同时有效的 grant 就可能重叠。即使两个注册串行执行，共享页
也仍可能被拒绝，所以单加串行锁不能消除该来源。

本地受控堆复现使用真实 transport 池初始化函数、真实 slot/qpair 类型及真实
`spdk_nvme_urma_register_memory()`，仅替换 allocator/provider。旧代码第 2 个
qpair 就出现 `Address range overlap` 和 `rc=-22`；修后 48 个同时存活的池通过。
这验证了页共享机制，未重放真实 UMMU 库的内部并发实现。

## 实现

新增内部 `spdk_urma_alloc_capsule_slots(count, slot_size)`：

- 起点对齐到 `max(4096, sysconf(_SC_PAGESIZE))`。
- 分配长度也向上取整到同一粒度，保证注册扩展后的范围完全位于自有分配内。
  仅对齐起点、却仍按未补齐的长度分配，会让尾页落到其他对象，仍不满足要求。
- 整个分配清零；检查零长度、乘法/取整溢出和分配失败。
- 使用普通 host `posix_memalign` / `free`，不新增 DMA 大页池或 URMA 注册。

四个注册池使用该分配器：

| 端 | 注册池 | 初始化函数 |
|---|---|---|
| target | command RX | `nvmf_urma_capsule_rx_post` |
| target | response TX | `nvmf_urma_rsp_tx_pool_init` |
| initiator | response RX | `nvme_urma_capsule_resources_init` |
| initiator | command TX | `nvme_urma_tx_pool_init` |

注册仍使用逻辑 `count * slot_size` 长度，由公共注册层生成 grant 范围；设备扇出、
token、wire 格式和注册次数不变。已有生命周期保持：先排空 WR/CR，再 unregister，
最后 free。initiator 断连时保留 slot 地址，重连重新注册，qpair 删除时才释放。
不把 slot 留给尚未排空的 CQE 的规则也保持。

当前 target 的 `stage=rx_init` 是整个 `capsule_resources_init()` 的错误标签，
该函数先初始化 TX，再检查 RX。TCP capsule 模式跳过 RX，但 bonding 仍可初始化
response TX 池；因此该日志标签本身不能证明具体是 RX 失败。历史二进制需按
其源码版本、实际 slot 大小及队列深度核对，本修复同时覆盖 RX 和 TX。

每池额外申请的补齐空间小于一个对齐页，此外还有 allocator 自身开销；不是 RSS
硬上限。qpair 数和队列深度不变，原 iobuf/whole-pool/application buffer 不改。
它们如果存在其他注册重叠，需要按各自所有权单独处理。

## 可重复验证

```bash
# 当前代码：通过（本地模拟，无远端或硬件）
python3 test/urma/capsule_pages_build.py \
  --umdk /home/senorita/GDR/GDR_Related/UMDK_netlab

# 取修复前 transport 函数：预期失败，保留同一个公共注册路径/provider 模型
python3 test/urma/capsule_pages_build.py \
  --umdk /home/senorita/GDR/GDR_Related/UMDK_netlab \
  --transport-ref b0cc96f0a --case target-rx

# 内存与生命周期检查
python3 test/urma/capsule_pages_build.py \
  --umdk /home/senorita/GDR/GDR_Related/UMDK_netlab --sanitize
```

覆盖 1/2/32/128 slots × 两端 RX/TX × 48 个同时存活的池；48 个 OS 线程进行
960 次双端 qpair 建立/释放；HELLO RX 幂等、同 qpair RX/TX 分离、十次重连、
4 KiB/64 KiB 主机页、分配/注册/部分 receive post 失败及 raw/TCP 分支。
测试检查实际 grant 位于分配内部、任意活跃 grant 不重叠、unregister 先于 free、
最后所有 grant/分配均归还。故障注入时的 `rc=-5` 日志是预期测试输出。

ASan/UBSan/LeakSanitizer 已通过；沙箱 ptrace 不支持 LSan，该轮在获授权的
沙箱外运行。本地 Admin 测试的 socketpair send 曾在沙箱内被 EPERM 拒绝，
获授权的沙箱外 ASan/UBSan/LSan 回归通过。

原 handshake（含 modern/legacy SDK 的完整 URMA transport 源码编译）、
whole-pool 与 Admin/IO Connect 回归通过。常规 `make -C lib/nvme -j2` 被本机缺少
`openssl/evp.h` 阻断（另有 pkg-config 缺失提示），因此不宣称完成了真实全库/
target/镜像构建。没有安装依赖或修改构建配置绕过该问题。

## Doc 65 方向的当前状态

| 方向 | 本轮状态 |
|---|---|
| UMDK/libummu 内部 grant 原子性/共享页语义 | 未修改，仍需库维护方验证 |
| SPDK 自有 qpair capsule 缓冲独占页 | 已实现，覆盖两端 RX/TX，本地回归通过 |
| client 分批启动、endpoint 退避重试/随机化 | 本轮未改 MP/Mooncake，属于独立接纳策略 |
| 显式 max-io-qpairs-per-ctrlr 预算 | 本轮未调整；不是页重叠的修复 |
| master attach fail-fast 退避重试 | 本轮未改，属于 Mooncake 独立问题 |

接下来由用户重编 SPDK target/client 并纳入 runtime 镜像，在原 bonding 配置下
复测 recall 48 并发及 rank+recall 并行，核对 target overlap/register_failed/rx_init、
client attach/qpair/whole-pool 成功数和应用内容校验。保留原 QPS/盘数等对照条件。
仍失败时记录具体 allocation/grant 范围与 context，区分其他 buffer 所有权问题和
provider 内部冲突。即使多轮全绿，也只能作为该版本场景验收，不能证明 libummu
内部竞态已经修复。当前没有执行远端访问、设备切换、部署或实机性能实验。
