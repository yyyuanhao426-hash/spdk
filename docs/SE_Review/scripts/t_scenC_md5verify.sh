#!/bin/bash
TRID='trtype:URMA adrfam:IPv4 traddr:141.61.84.245 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test'
export LD_LIBRARY_PATH=/home/tong/umdk_gpu_isolated/lib
export SPDK_URMA_DEV_NAME=udmac0d1e2
BIN=/home/yin/nof/spdk/build/examples/persist_check
echo "=== WRITE 32MB pattern (8192x4KB) ==="
$BIN -r "$TRID" -w write -o 4096 -l 0 -c 8192 2>&1 | tail -5
echo "=== VERIFY read-back ==="
$BIN -r "$TRID" -w verify -o 4096 -l 0 -c 8192 2>&1 | tail -5
echo "=== VERIFY2 128KB x 8192 = 1GB at LBA 1M ==="
$BIN -r "$TRID" -w write -o 131072 -l 1048576 -c 8192 2>&1 | tail -3
$BIN -r "$TRID" -w verify -o 131072 -l 1048576 -c 8192 2>&1 | tail -3
