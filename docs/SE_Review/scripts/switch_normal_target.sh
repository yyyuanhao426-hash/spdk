#!/bin/bash
TS=/tmp/se_reg
mkdir -p $TS
echo "$(date +%s) switch_to_normal_start" >> $TS/rounds.log
PID=$(cat /var/tmp/nvmf_tgt.pid 2>/dev/null)
echo "killing $PID" >> $TS/rounds.log
[ -n "$PID" ] && kill -9 $PID 2>/dev/null
sleep 3
cd /home/yin/nof/spdk || exit 1
echo "$(date +%s) restore" >> $TS/rounds.log
./target_nvme_takeover.sh -r -y > $TS/restore.log 2>&1
ROOT_OK=$(findmnt -no SOURCE / | head -1)
echo "root_src=$ROOT_OK" >> $TS/rounds.log
if echo "$ROOT_OK" | grep -q nvme7; then
  echo "ABORT root disk issue" >> $TS/rounds.log
  exit 1
fi
echo "$(date +%s) takeover_normal" >> $TS/rounds.log
./target_nvme_takeover.sh -d nvme7n1 -y -i 141.61.84.245 -L /home/tong/umdk_gpu_isolated/lib > $TS/takeover.log 2>&1
echo "$(date +%s) target_up" >> $TS/rounds.log
sleep 15
TGT_PID=$(cat /var/tmp/nvmf_tgt.pid 2>/dev/null)
LISTEN=$(ss -tlnp | grep -c 4420)
echo "tgt_pid=$TGT_PID listen_4420=$LISTEN" >> $TS/rounds.log
ls -l /proc/$TGT_PID/cwd 2>/dev/null >> $TS/rounds.log
echo "SWITCH_DONE" >> $TS/rounds.log
