#!/bin/bash
OUT=/tmp/se_c/reconnect_rss.csv
mkdir -p /tmp/se_c
echo "ts_epoch rss_kb" > $OUT
while true; do
  P=$(awk '{print $2}' /tmp/se_c/reconnect.pid 2>/dev/null)
  if [ -n "$P" ] && [ -d /proc/$P ]; then
    RSS=$(grep VmRSS /proc/$P/status 2>/dev/null | awk '{print $2}')
    echo "$(date +%s) $RSS" >> $OUT
  else
    echo "$(date +%s) NO_RECONNECT" >> $OUT
  fi
  sleep 2
done
