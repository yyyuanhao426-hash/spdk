#!/bin/bash
TS=/tmp/se_e
mkdir -p $TS
echo "$(date +%s) scenE_start" > $TS/rounds.log

# 1. 并发半开连接：5 个 nc 连 4420，不发 HELLO，挂 8 秒
for i in 1 2 3 4 5; do
  (sleep 8 | nc 141.61.84.245 4420 > /dev/null 2>&1) &
  echo "  nc_$i started pid=$!" >> $TS/rounds.log
done
NC_PIDS=$(jobs -p)
echo "$(date +%s) nc_connections_open n=$(jobs | wc -l)" >> $TS/rounds.log

# 2. 等 2 秒让半开连接建立，期间 target 必须仍能正常 accept
sleep 2
echo "$(date +%s) check_target_alive_before" >> $TS/rounds.log
# 正常连接探活：完整 URMA 握手（用 nc 发垃圾会触发握手失败，但证明 accept 未卡）
(echo -n "X" | nc -w 2 141.61.84.245 4420 > /dev/null 2>&1) &
echo "  probe_conn sent" >> $TS/rounds.log

# 3. 等待半开连接超时（5s deadline）被关闭
sleep 6
echo "$(date +%s) after_deadline_wait" >> $TS/rounds.log

# 4. 确认 target 进程与 listener 仍正常
ps aux | grep nvmf_tgt | grep -v grep >> $TS/rounds.log
ss -tlnp | grep 4420 >> $TS/rounds.log
echo "$(date +%s) scenE_done" >> $TS/rounds.log
wait
echo "SCEN_E_LOCAL_DONE" >> $TS/rounds.log
