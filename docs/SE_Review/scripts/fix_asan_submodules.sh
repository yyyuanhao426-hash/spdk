#!/bin/bash
cd /home/yin/nof/spdk_asan || exit 1
for m in dpdk intel-ipsec-mb isa-l isa-l-crypto libvfio-user ocf xnvme; do
  rm -rf "$m"
  ln -sfn /home/yin/nof/spdk/$m "$m"
  echo "linked $m -> /home/yin/nof/spdk/$m"
done
echo "---verify---"
for m in dpdk intel-ipsec-mb isa-l isa-l-crypto libvfio-user ocf xnvme; do
  if [ -L "$m" ]; then echo "$m: $(readlink "$m")"; else echo "$m: FAIL"; fi
done
