#!/bin/bash
RUNDIR=/tmp/se_b
mkdir -p $RUNDIR
N=${N:-50}
for i in $(seq 1 $N); do
  SPDK_URMA_DEV_NAME=udmac0d1e2 LD_LIBRARY_PATH=/home/tong/umdk_gpu_isolated/lib \
    /home/yin/nof/spdk/build/examples/urma_perf \
    -r 'trtype:URMA adrfam:IPv4 traddr:141.61.84.245 trsvcid:4420 subnqn:nqn.2026-01.io.spdk:urma-gpu-test' \
    -w write -o 4096 -T 4 -b 32 -t 30 -n 1 -g 0 -l 0 -M cpu \
    > $RUNDIR/urma_kill_$i.log 2>&1 &
  PID=$!
  sleep $((RANDOM % 8 + 2))
  kill -9 $PID 2>/dev/null
  echo "$(date +%s) round=$i killed=$PID" >> $RUNDIR/rounds.log
  sleep 3
done
echo "LOOP_DONE $(date +%s)" >> $RUNDIR/rounds.log
