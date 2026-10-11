#!/bin/bash
echo "=== fio version ==="
fio --version 2>&1
echo "=== spdk fio plugin ==="
ls -la /home/yin/nof/spdk/build/fio/spdk_nvme 2>&1
ls -la /home/yin/nof/spdk/build/fio/*.so 2>&1 | head -5
find /home/yin/nof/spdk/build -name '*fio*' -type f 2>/dev/null | head -5
echo "=== fio plugin list ==="
fio --enghelp 2>/dev/null | grep -i spdk
