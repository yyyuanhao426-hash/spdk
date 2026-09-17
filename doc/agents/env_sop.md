# NDS 环境搭建 SOP（换新机器标准流程）

> 适用场景：测试机器被占用/不稳定，切换到新机器时，照本文从零走到
> "urma_perf 可运行"。全程遵守主对话文件 0.5 节隔离守则。
> 每个阶段的全部输出**原样**回传主对话文件第 9 节。

- [ ] 阶段 0：机器角色识别
- [ ] 阶段 1：环境体检
- [ ] 阶段 2：代码部署
- [ ] 阶段 3：gds UMDK 部署
- [ ] 阶段 4：编译
- [ ] 阶段 5：产物验证
- [ ] 回传报告，等待外部解锁验证批次

---

## 阶段 0：机器角色识别

```bash
hostname; ip a | grep "inet " | grep -v 127.0.0.1
npu-smi info        # 有表格 = NPU 机（Initiator 候选）
nvme list           # 有盘 = Target 候选
```

- [ ] 记录两台的主机名/IP/角色

## 阶段 1：环境体检（只读）

### 1.1 两台公共

```bash
uname -r && cat /etc/os-release | head -3 && uname -m
lsmod | grep -E "udma|urma|ubus|ubcore|ubase|ummu"
for m in ubcore uburma udma ummu ummu_core ubus ubase; do \
  echo "== $m =="; modinfo $m 2>/dev/null | grep -E "^(filename|version|srcversion)"; done
which gcc make git; gcc --version | head -1
curl -sI --max-time 10 https://github.com | head -3
df -h /home | tail -1
```

- [ ] 记录内核版本/架构/驱动 srcversion（两台对比，历史问题复检点）
- [ ] 记录外网连通性（决定代码用 clone 还是离线拷贝）

### 1.2 NPU 机专查

```bash
npu-smi info
find /usr/local/Ascend -maxdepth 3 -name "libascendcl.so" 2>/dev/null
nm -D <libascendcl.so> | grep -iE "Export|Shareable|MallocPhysical|ImportFrom" | head -20
cat /proc/kallsyms | grep -E "vdavinci|davinci.*pin" | head -20
```

- [ ] NPU 型号/数量、CANN 版本与路径、导出接口、davinci 符号导出情况

### 1.3 Target 机专查

```bash
nvme list; lsblk -o NAME,MOUNTPOINT,TYPE,FSTYPE,SIZE
```

- [ ] 标出空闲盘（盘名+BDF）、系统盘（禁碰）、md/LVM 成员盘

### 1.4 双机互通

```bash
ping -c 3 <对方 IP>
```

- [ ] 记录互通性；向用户确认是否共用机

## 阶段 2：代码部署（NPU 机）

```bash
# 有外网：
git clone -b nds_v1 https://github.com/yyyuanhao426-hash/spdk.git <home>/nds/spdk
git log --oneline -1          # 记录 HEAD
# 无外网：从有网机器 git pull 后 scp 整目录过来（禁用 Windows 文本传输，防 CRLF）
```

- [ ] `git log --oneline -1` 输出已记录

## 阶段 3：gds UMDK 部署（NPU 机 + Target 机）

gds 版 UMDK（含 is_gpu_seg / urma_register_seg_dmabuf 扩展）是编译与运行的
必备依赖。来源按优先级：

```bash
# ① 从仍保留 UMDK 的旧机器 rsync/scp（同为 aarch64，二进制可直接复用）
scp -r <旧机器>:<UMDK 路径> <home>/nds/UMDK_netlab
# ② 均无则从 AtomGit 拉源码自行编译（分支 sp4_umdk）
```

- [ ] 验证扩展齐全：

```bash
grep -rn "is_gpu_seg" <UMDK>/src/urma/lib/urma/core/include/ | head -3
nm -D <UMDK>/lib/liburma.so | grep register_seg
```

- [ ] 记录 UMDK 路径（后续 LD_LIBRARY_PATH 用它）

## 阶段 4：编译（NPU 机）

```bash
cd <home>/nds/spdk
./configure --with-urma=<UMDK 路径> 2>&1 | tail -20
nice -n 10 make -j16 2>&1 | tail -30
```

- [ ] configure/make 成功；若报错完整回传
- [ ] Target 机同样需要一份编译好的 nvmf_tgt（重复阶段 2-4 或拷贝产物）

## 阶段 5：产物验证

```bash
ls -l build/examples/urma_perf build/bin/nvmf_tgt
./build/examples/urma_perf -h 2>&1 | grep -A3 -- '-M'
# 启动输出应含 "CANN runtime loaded: <路径>"（记录实际加载的 CANN 版本）
```

- [ ] urma_perf 帮助文本含 npu / npu-staged
- [ ] 阶段 1-5 全部完成 → 回传报告，注明「环境就绪，等待验证批次」

## 常见问题速查

| 症状 | 原因 | 处理 |
|------|------|------|
| `CONFIG.sh: $'\r': command not found` | Windows 传输引入 CRLF | 改用 git clone / Linux scp |
| `undefined symbol: urma_register_seg_dmabuf` | 运行时加载了标准 liburma | host-only 路线已兼容（dlsym）；若仍报错回传 |
| `aclrtSetDevice failed 507033` | URMA/DPDK 环境使 HDC 走错通道（历史问题，新环境可能不复现） | 原样记录 + CANN 调试日志（ASCEND_GLOBAL_LOG_LEVEL=0） |
| `Invalid URMA host metadata` | 两端 SPDK 版本不一致 | 确认两端都从 nds_v1 同一 HEAD 构建 |
| md 文件中文乱码 | 用了会转码的工具改写 | 用 git/文本安全工具 |
