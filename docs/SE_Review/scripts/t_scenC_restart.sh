#!/bin/bash
TS=/tmp/se_c
mkdir -p $TS
N=${N:-10}
cd /home/yin/nof/spdk_asan || exit 1
for i in $(seq 1 $N); do
  echo "$(date +%s) round=$i kill_target_start" >> $TS/rounds.log
  PID=$(cat /var/tmp/nvmf_tgt.pid 2>/dev/null)
  [ -n "$PID" ] && kill -9 $PID 2>/dev/null
  sleep 3
  echo "$(date +%s) round=$i restore" >> $TS/rounds.log
  ./target_nvme_takeover.sh -r -y > $TS/restore_$i.log 2>&1
  lsblk | grep -E 'nvme7|nvme[0-9]+n[0-9]+ .*/$|/ ' > $TS/lsblk_$i.log
  ROOT_OK=$(findmnt -no SOURCE / | head -1)
  echo "root_src=$ROOT_OK" >> $TS/lsblk_$i.log
  echo "$(date +%s) round=$i takeover" >> $TS/rounds.log
  ./target_nvme_takeover.sh -d nvme7n1 -y -i 141.61.84.245 -L /home/tong/umdk_gpu_isolated/lib > $TS/takeover_$i.log 2>&1
  echo "$(date +%s) round=$i target_up" >> $TS/rounds.log
  sleep 15
  echo "$(date +%s) round=$i wait_done" >> $TS/rounds.log
done
echo "SCEN_C_DONE $(date +%s)" >> $TS/rounds.log
