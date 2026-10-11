#!/bin/bash
OUT=/tmp/se_a/target_rss.csv
mkdir -p /tmp/se_a
echo "ts_epoch rss_kb" > $OUT
while true; do
  P=$(cat /var/tmp/nvmf_tgt.pid 2>/dev/null)
  if [ -n "$P" ] && [ -d /proc/$P ]; then
    RSS=$(grep VmRSS /proc/$P/status 2>/dev/null | awk '{print $2}')
    echo "$(date +%s) $RSS" >> $OUT
  else
    echo "$(date +%s) NO_TARGET" >> $OUT
  fi
  sleep 2
done
