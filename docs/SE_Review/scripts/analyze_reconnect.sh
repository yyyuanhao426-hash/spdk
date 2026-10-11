#!/bin/bash
cd /home/yin/nof/spdk
echo "--- case R ---"
grep -n -A3 "case 'R'" examples/nvme/reconnect/reconnect.c
echo "--- max_ctrlr_resets ---"
grep -n 'max_ctrlr_resets' examples/nvme/reconnect/reconnect.c
echo "--- TIMELINE ---"
grep -E 'disconnecting|failed with error|Exiting|reset|Initialization|Attached' /tmp/se_c/reconnect.log | sed 's/\[2026-10-11 //' | head -50
