#!/bin/bash
TS=/tmp/se_c
mkdir -p $TS
SPDK_URMA_DEV_NAME=udmac0d1e2 LD_LIBRARY_PATH=/home/tong/umdk_gpu_isolated/lib \
  /home/yin/nof/spdk/build/examples/reconnect \
  -q 32 -o 4096 -w write -t 1500 \
  -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.245 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
  > $TS/reconnect.log 2>&1 &
echo "RECONNECT_PID $!" > $TS/reconnect.pid
echo "RECONNECT_STARTED $(date +%s)" >> $TS/rounds.log
