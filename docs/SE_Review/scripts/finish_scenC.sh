#!/bin/bash
awk 'NR>1 && $2!="NO_RECONNECT" {if(min==""){min=$2;max=$2}; if($2<min)min=$2; if($2>max)max=$2; n++; sum+=$2} END {printf "samples=%d min=%d max=%d avg=%.0f\n", n, min, max, sum/n}' /tmp/se_c/reconnect_rss.csv
echo "=== NO_RECONNECT count ==="
grep -c NO_RECONNECT /tmp/se_c/reconnect_rss.csv
kill 3021210 2>/dev/null
sleep 2
ps -p 3021210 -o pid,stat 2>&1 | tail -1
echo "=== final reconnect log tail ==="
tail -3 /tmp/se_c/reconnect.log
