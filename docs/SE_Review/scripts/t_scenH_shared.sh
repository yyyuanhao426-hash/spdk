#!/bin/bash
cd /home/yin/nof/spdk_shared || exit 1
LOG=/tmp/se_h_build.log
: > $LOG
echo "CONFIGURE_START $(date +%s)" >> $LOG
./configure --with-urma=/home/tong/UMDK_netlab --with-dpdk=/home/yin/nof/spdk/dpdk/build --with-shared >> $LOG 2>&1
if [ $? -ne 0 ]; then echo "CONFIGURE_FAIL $(date +%s)" >> $LOG; tail -20 $LOG; exit 1; fi
echo "CONFIGURE_OK $(date +%s)" >> $LOG
echo "MAKE_START $(date +%s)" >> $LOG
make -j$(nproc) >> $LOG 2>&1
if [ $? -ne 0 ]; then echo "MAKE_FAIL $(date +%s)" >> $LOG; tail -30 $LOG; exit 1; fi
echo "MAKE_OK $(date +%s)" >> $LOG
echo "SYMBOL_CHECK_START $(date +%s)" >> $LOG
nm -D build/lib/libspdk_nvme.so 2>/dev/null | grep -E 'spdk_nvme_urma_region_registry_(add|find)' >> $LOG
echo "registry_exported_count=$(nm -D build/lib/libspdk_nvme.so 2>/dev/null | grep -cE 'T spdk_nvme_urma_')" >> $LOG
echo "legacy_unexported=$(nm -D build/lib/libspdk_nvme.so 2>/dev/null | grep -cE 'nvme_urma_region_registry')" >> $LOG
echo "SYMBOL_CHECK_DONE $(date +%s)" >> $LOG
echo "SHARED_BUILD_DONE $(date +%s)" >> $LOG
