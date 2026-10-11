#!/bin/bash
cd /home/yin/nof/spdk_shared || exit 1
for m in dpdk intel-ipsec-mb isa-l isa-l-crypto libvfio-user ocf xnvme; do
  if [ ! -L "$m" ]; then
    rm -rf "$m"
    ln -sfn /home/yin/nof/spdk/$m $m
    echo "linked $m"
  else
    echo "already symlink: $m"
  fi
done
echo "---verify---"
for m in dpdk intel-ipsec-mb isa-l isa-l-crypto libvfio-user ocf xnvme; do
  stat -c '%F %n' "$m"
done
