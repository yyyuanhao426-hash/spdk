#!/bin/bash
echo "=== fio src dirs ==="
ls -d /usr/src/fio* /opt/fio* /home/yin/fio* /root/fio* 2>/dev/null
which fio
echo "=== spdk fio config ==="
grep -i fio /home/yin/nof/spdk/config.mk 2>/dev/null | head -5
grep -ri fio /home/yin/nof/spdk/mk/config.mk 2>/dev/null | head -5
echo "=== spdk fio makefile ==="
ls /home/yin/nof/spdk/fio/ 2>/dev/null | head -10
echo "=== CONFIG options ==="
grep -E 'FIO|fio' /home/yin/nof/spdk/build/../config.mk 2>/dev/null | head -3
grep -E 'FIO' /home/yin/nof/spdk/mk/config.mk 2>/dev/null
