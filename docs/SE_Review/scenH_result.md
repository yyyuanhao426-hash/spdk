# 场景 H — shared 构建 + registry 符号导出（SE_Review #8）

**日期**：2026-10-11
**验证对象**：2e46861e4 提交 —— registry 改名 `spdk_nvme_urma_*` 进入现有 map 通配导出

## 结论

**PASS** — `--with-shared` 构建完整通过；`libspdk_nvme.so` 动态符号表正确导出 target 跨库依赖的两个 registry 函数；shared 版 nvmf_tgt 冒烟启动正常。

## 方法与结果

node1 新建 shared worktree（`/home/yin/nof/spdk_shared`，HEAD fba60e72e），`./configure --with-urma=/home/tong/UMDK_netlab --with-dpdk=/home/yin/nof/spdk/dpdk/build --with-shared && make -j$(nproc)`。

### 符号检查（`nm -D build/lib/libspdk_nvme.so`）

```
000000000005a2e0 T spdk_nvme_urma_region_registry_add
000000000005a490 T spdk_nvme_urma_region_registry_find
000000000005a3e4 T spdk_nvme_urma_region_registry_remove
```

- registry_add/find 均为 `T`（text 导出）——修复前这两个符号名 `nvme_urma_region_registry_*` 不匹配 map 中 `spdk_nvme_urma_*`/`spdk_urma_*` 通配，且被末尾 `local: *;` 隐藏，shared 链接/加载必失败
- 全部 `spdk_nvme_urma_*` 导出符号共 14 个，无遗留未导出的 `nvme_urma_region_registry` 裸符号

### 冒烟

- `ldd build/bin/nvmf_tgt`（LD_LIBRARY_PATH=build/lib + umdk_gpu_isolated）：全部解析，无 `not found`，URMA 库正确指向 `/home/tong/umdk_gpu_isolated/lib`
- `nvmf_tgt --version` → `SPDK v26.09-pre git sha1 fba60e72e`（可执行）

## 构建环境修复记录

- 新版 configure 参数为 `--with-shared`（非 `--enable-shared`）
- git worktree 会为子模块生成**占位空目录**而非符号链接，需逐一删除并 `ln -sfn /home/yin/nof/spdk/<m> <m>`（dpdk/intel-ipsec-mb/isa-l/isa-l-crypto/libvfio-user/ocf/xnvme 共 7 个）
- shared 树同样包含 GLM fba60e72e 引入的 urma.c 重复成员 bug（407-408 行），按已授权方案备份+删除（`urma.c.bak_fini`）
