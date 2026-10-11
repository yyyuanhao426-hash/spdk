#!/bin/bash
TS=/tmp/se_d
mkdir -p $TS
cd /home/yin/nof/spdk_asan || exit 1
echo "$(date +%s) kill_target_start" >> $TS/rounds.log
PID=$(cat /var/tmp/nvmf_tgt.pid 2>/dev/null)
[ -n "$PID" ] && kill -9 $PID 2>/dev/null
sleep 3
echo "$(date +%s) restore" >> $TS/rounds.log
./target_nvme_takeover.sh -r -y > $TS/restore.log 2>&1
lsblk | grep -E 'nvme' > $TS/lsblk_after_restore.log
ROOT_OK=$(findmnt -no SOURCE / | head -1)
echo "root_src=$ROOT_OK" >> $TS/lsblk_after_restore.log
if [ -z "$ROOT_OK" ] || echo "$ROOT_OK" | grep -q nvme7; then
  echo "$(date +%s) ABORT_ROOT_DISK_ISSUE root_src=$ROOT_OK" >> $TS/rounds.log
  exit 1
fi
echo "$(date +%s) takeover" >> $TS/rounds.log
./target_nvme_takeover.sh -d nvme7n1 -y -i 141.61.84.245 -L /home/tong/umdk_gpu_isolated/lib > $TS/takeover.log 2>&1
echo "$(date +%s) target_up" >> $TS/rounds.log
sleep 15
TGT_PID=$(cat /var/tmp/nvmf_tgt.pid 2>/dev/null)
LISTEN=$(ss -tlnp | grep -c 4420)
echo "tgt_pid=$TGT_PID listen_4420=$LISTEN" >> $TS/rounds.log
echo "SCEN_D_RESTART_DONE $(date +%s)" >> $TS/rounds.log
