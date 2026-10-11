#!/bin/bash
TRID='trtype:URMA adrfam:IPv4 traddr:141.61.84.245 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test'
export LD_LIBRARY_PATH=/home/tong/umdk_gpu_isolated/lib
export SPDK_URMA_DEV_NAME=udmac0d1e2
BIN=/home/yin/nof/spdk/build/examples/persist_check
echo "=== VERIFY 1GB (128KB x 8192 at LBA 1M) ==="
$BIN -r "$TRID" -w verify -o 131072 -l 1048576 -c 8192 2>&1 | grep -vE 'Unknown transport|shutdown_get_cc|CQ transport error|in failed state' | tail -8
