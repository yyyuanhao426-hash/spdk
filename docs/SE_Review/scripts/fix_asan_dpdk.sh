#!/bin/bash
set -e
CFG=/home/yin/nof/spdk_asan/mk/config.mk
sed -i 's|^CONFIG_DPDK_DIR?=/home/yin/nof/spdk/dpdk$|CONFIG_DPDK_DIR?=/home/yin/nof/spdk/dpdk/build|' $CFG
sed -i 's|^CONFIG_DPDK_LIB_DIR?=/home/yin/nof/spdk/dpdk/lib$|CONFIG_DPDK_LIB_DIR?=|' $CFG
sed -i 's|^CONFIG_DPDK_INC_DIR?=/home/yin/nof/spdk/dpdk/include$|CONFIG_DPDK_INC_DIR?=|' $CFG
grep -n "CONFIG_DPDK" $CFG | head -5
