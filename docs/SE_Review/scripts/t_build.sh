#!/bin/bash
set -x
cd /home/yin/nof/spdk || exit 1
make clean 2>/dev/null || true
./configure --with-urma=/home/tong/UMDK_netlab > /tmp/se_build_configure.log 2>&1 || { echo CONFIGURE_FAIL >> /tmp/se_build_outer.log; tail -30 /tmp/se_build_configure.log >> /tmp/se_build_outer.log; exit 1; }
echo CONFIGURE_OK >> /tmp/se_build_outer.log
make -j$(nproc) > /tmp/se_build_make.log 2>&1 || { echo MAKE_FAIL >> /tmp/se_build_outer.log; tail -30 /tmp/se_build_make.log >> /tmp/se_build_outer.log; exit 1; }
echo MAKE_OK >> /tmp/se_build_outer.log
make -C examples/nvme/urma_perf >> /tmp/se_build_make.log 2>&1 || { echo PERF_FAIL >> /tmp/se_build_outer.log; tail -20 /tmp/se_build_make.log >> /tmp/se_build_outer.log; exit 1; }
echo PERF_OK >> /tmp/se_build_outer.log
make -C examples/nvme/reconnect >> /tmp/se_build_make.log 2>&1 || { echo RECONNECT_FAIL >> /tmp/se_build_outer.log; tail -20 /tmp/se_build_make.log >> /tmp/se_build_outer.log; exit 1; }
echo RECONNECT_OK >> /tmp/se_build_outer.log
ls -la build/bin/nvmf_tgt build/examples/urma_perf build/examples/reconnect/reconnect >> /tmp/se_build_outer.log
ldd build/bin/nvmf_tgt | grep urma >> /tmp/se_build_outer.log
strings build/bin/nvmf_tgt | grep -c "drain deadline exceeded" >> /tmp/se_build_outer.log
strings build/bin/nvmf_tgt | grep -c "response reached with CID=0" >> /tmp/se_build_outer.log
strings build/bin/nvmf_tgt | grep -c "HELLO_RECV" >> /tmp/se_build_outer.log
strings build/examples/urma_perf | grep -c "epoch" >> /tmp/se_build_outer.log
echo BUILD_DONE_$? >> /tmp/se_build_outer.log
