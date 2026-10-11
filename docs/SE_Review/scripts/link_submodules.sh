#!/bin/bash
cd /home/yin/nof/spdk_shared || exit 1
for m in dpdk isa-l; do
  if [ ! -d "$m" ] || [ -z "$(ls "$m" 2>/dev/null)" ]; then
    ln -sfn /home/yin/nof/spdk/$m $m && echo "linked $m"
  else
    echo "already ok: $m"
  fi
done
ls -la dpdk/ 2>&1 | head -5
ls dpdk/build/lib 2>/dev/null | head -3
