#!/bin/bash
cd /home/yin/nof/spdk || exit 1
sed -n '956,965p' examples/nvme/reconnect/reconnect.c
echo "=== apply fix: reset num_resets on successful reset ==="
sed -i 's/\t\t\t\tfprintf(stderr, "Controller properly reset.\\n");/\t\t\t\tentry->num_resets = 0;\n\t\t\t\tfprintf(stderr, "Controller properly reset.\\n");/' examples/nvme/reconnect/reconnect.c
sed -n '956,968p' examples/nvme/reconnect/reconnect.c
