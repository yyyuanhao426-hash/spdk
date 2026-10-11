#!/bin/bash
cd /home/yin/nof/spdk_asan || exit 1
for m in dpdk isa-l isa-l-crypto intel-ipsec-mb libvfio-user ocf xnvme; do
  if [ -L "$m" ]; then
    echo "$m: SYMLINK -> $(readlink "$m")"
  elif [ -d "$m" ]; then
    echo "$m: DIR (not link) $(ls "$m" | head -2 | tr '\n' ' ')"
  else
    echo "$m: MISSING"
  fi
done
