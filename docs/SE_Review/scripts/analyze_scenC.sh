#!/bin/bash
LOG=/tmp/se_c/reconnect.log
echo "=== per-round events (fatal -> reset result) ==="
grep -nE 'disconnecting for reconnect|properly reset|Cannot be recovered|Attached|Initialization complete' $LOG | sed 's/\[2026-10-11 //;s/\] .*nvme_[a-z]*\.c:[0-9]*:/\] /' | awk '{printf "%s %s %s\n", $1, $2, $3}'
echo "=== RSS range ==="
awk 'NR>1 && $2!="NO_RECONNECT" {if(min==""){min=$2;max=$2}; if($2<min)min=$2; if($2>max)max=$2; n++; sum+=$2} END {printf "samples=%d min=%d max=%d avg=%.0f\n", n, min, max, sum/n}' /tmp/se_c/reconnect_rss.csv
echo "=== final lines ==="
tail -5 $LOG
