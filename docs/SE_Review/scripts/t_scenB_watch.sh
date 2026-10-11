#!/bin/bash
OUT=/tmp/se_b/asan_watch.log
mkdir -p /tmp/se_b
echo "watch_start $(date +%s)" > $OUT
while true; do
  if [ -d /proc/742200 ]; then
    A=$(grep -cE 'ERROR: AddressSanitizer|heap-use-after-free|use-after-free|stack-buffer|global-buffer' /tmp/nvmf_tgt.log)
    if [ "$A" -gt 0 ]; then
      echo "$(date +%s) ASAN_HIT n=$A" >> $OUT
      break
    fi
  else
    echo "$(date +%s) TARGET_DIED" >> $OUT
    break
  fi
  sleep 1
done
echo "watch_end $(date +%s)" >> $OUT
