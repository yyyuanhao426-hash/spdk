#!/bin/bash
set -x
cd /home/yin/nof/spdk_asan || exit 1
make clean >/dev/null 2>&1 || true
./configure --with-urma=/home/tong/UMDK_netlab --with-dpdk=/home/yin/nof/spdk/dpdk/build --enable-asan > /tmp/se_build_asan_configure.log 2>&1 || { echo CONFIGURE_FAIL >> /tmp/se_build_asan_outer.log; tail -20 /tmp/se_build_asan_configure.log >> /tmp/se_build_asan_outer.log; exit 1; }
echo CONFIGURE_OK >> /tmp/se_build_asan_outer.log
make -j$(nproc) > /tmp/se_build_asan_make.log 2>&1 || { echo MAKE_FAIL >> /tmp/se_build_asan_outer.log; tail -30 /tmp/se_build_asan_make.log >> /tmp/se_build_asan_outer.log; exit 1; }
echo MAKE_OK >> /tmp/se_build_asan_outer.log
make -C examples/nvme/urma_perf >> /tmp/se_build_asan_make.log 2>&1 || { echo PERF_FAIL >> /tmp/se_build_asan_outer.log; tail -20 /tmp/se_build_asan_make.log >> /tmp/se_build_asan_outer.log; exit 1; }
echo PERF_OK >> /tmp/se_build_asan_outer.log
ldd build/bin/nvmf_tgt | grep -c asan >> /tmp/se_build_asan_outer.log
echo BUILD_ASAN_DONE_$? >> /tmp/se_build_asan_outer.log
