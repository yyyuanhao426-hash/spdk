#!/bin/bash
# =============================================================================
# URMA 消息尺寸扫描 (仿 check_urma.sh 测试方式, bash 3.x 兼容)
# 功能：在同一对节点上用 urma_perftest write_bw 扫不同消息尺寸，
#       对照 SPDK v6 实测 ~38.5 GiB/s，定位 46 vs 38.5 的差距在哪一层
#
# 测试方式与 check_urma.sh 完全一致（server nohup + client -S <ip>），
# 只有 -s 消息尺寸变化 —— 所以 64KB 行应直接复现 47148 MB/s 那个数，
# 两者的差值可归因于消息尺寸而非测试方法。
#
# 用法：
#   ./urma_size_sweep.sh                          # node3→node2 + node2→node3 双向
#   DEVICE=bonding_dev_0 ./urma_size_sweep.sh     # 换 bonding 设备
#   SIZES="65536 4194304" ./urma_size_sweep.sh    # 只测部分尺寸
#   SWEEP_ONE_WAY=1 ./urma_size_sweep.sh          # 只测 node3→node2 单向
#
# 前提：与 check_urma.sh 相同 —— 两个节点的 urma_perftest 在 PATH 中，
#       执行机装了 sshpass 且能 root SSH 到两个节点。
# =============================================================================

set -u

# ======================== 配置区 ========================
CLIENT_NODE_IP="${CLIENT_NODE_IP:-141.61.84.149}"    # node3（SPDK 测试的 initiator 侧）
SERVER_NODE_IP="${SERVER_NODE_IP:-141.61.84.247}"    # node2（SPDK 测试的 target 侧）
CLIENT_NAME="${CLIENT_NAME:-node3}"
SERVER_NAME="${SERVER_NAME:-node2}"
DEVICE="${DEVICE:-udmac0d1e2}"
INLINE_SIZE=128
SIZES="${SIZES:-65536 262144 1048576 2097152 4194304}"
URMA_PERF="${URMA_PERF:-urma_perftest}"
TIMEOUT=60                # 单次 client 超时(秒)；大消息给足余量，健康运行远早于此结束
SERVER_READY_WAIT=5       # server 就绪等待重试次数(秒)
SSH_PORT=22
SSH_USER="root"
SSH_PASS="${SSH_PASS:-Huawei12#$}"
LOG_DIR="/tmp/urma_size_sweep_logs"
ONE_WAY="${SWEEP_ONE_WAY:-0}"

# 历史基线：check_urma.sh 在 node2↔node3, udmac0d1e2, -s 65536 的实测值
BASELINE_MBPS="${BASELINE_MBPS:-47148}"
# SPDK v6 对照：9SSD 4MB pull, -b32 -T2, node3 initiator → node2 target
SPDK_GIBPS="${SPDK_GIBPS:-38.5}"

RESULT_FILE="/tmp/urma_size_sweep_results_$$"

# ======================== 函数区 ========================
log_info() { echo "[INFO] $*"; }
log_ok()   { echo "[OK]   $*"; }
log_fail() { echo "[FAIL] $*"; }

size_label() {
    local s=$1
    if [ "${s}" -ge 1048576 ]; then
        awk "BEGIN{printf \"%gMB\", ${s}/1048576}"
    else
        awk "BEGIN{printf \"%gKB\", ${s}/1024}"
    fi
}

record() {
    # 方向|尺寸|带宽（带宽为空串/状态字时不参与汇总换算）
    echo "$1|$2|$3" >> "${RESULT_FILE}"
}

# SSH 远程执行（同 check_urma.sh：sshpass + 关闭 hostkey 检查）
ssh_exec() {
    local host="$1"
    local cmd="$2"
    local err_file="/tmp/urma_sweep_ssh_err_$$"
    local output
    output=$(sshpass -p "${SSH_PASS}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 \
        -p "${SSH_PORT}" "${SSH_USER}@${host}" "${cmd}" 2>"${err_file}")
    local ret=$?
    rm -f "${err_file}"
    echo "$output"
    return $ret
}

# SSH 远程后台启动 server（同 check_urma.sh 两步法：base64 落脚本再 nohup，
# 避免 SSH 后台重定向挂住）
ssh_exec_bg() {
    local host="$1"
    local cmd="$2"
    local err_file="/tmp/urma_sweep_ssh_err_$$"
    local rscript="/tmp/urma_launch_sweep.sh"
    local logfile="/tmp/urma_sweep_server_${host}.log"

    local cmd_b64
    cmd_b64=$(printf '%s' "${cmd}" | base64 | tr -d '\n')
    sshpass -p "${SSH_PASS}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 \
        -p "${SSH_PORT}" "${SSH_USER}@${host}" "echo '${cmd_b64}' | base64 -d > ${rscript} && chmod +x ${rscript}" 2>"${err_file}"
    local ret1=$?
    if [ $ret1 -ne 0 ]; then
        rm -f "${err_file}"
        echo ""
        return 1
    fi

    local output
    output=$(sshpass -p "${SSH_PASS}" ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 \
        -p "${SSH_PORT}" "${SSH_USER}@${host}" "nohup ${rscript} > ${logfile} 2>&1 < /dev/null & echo \$!" 2>"${err_file}")
    local ret2=$?
    rm -f "${err_file}"
    echo "$output"
    return $ret2
}

check_sshpass() {
    if ! command -v sshpass >/dev/null 2>&1; then
        log_fail "sshpass 未安装! 请执行: yum install -y sshpass（或配免密后改用普通 ssh）"
        exit 1
    fi
}

cleanup_node() {
    local ip="$1"
    ssh_exec "${ip}" "pkill -f 'urma_perftest' 2>/dev/null" >/dev/null 2>&1
    return 0
}

wait_server_ready() {
    local server_ip="$1"
    local i=0
    while [ $i -lt ${SERVER_READY_WAIT} ]; do
        local alive
        alive=$(ssh_exec "${server_ip}" "pgrep -f 'urma_perftest' >/dev/null 2>&1 && echo ALIVE || echo DEAD" 2>/dev/null)
        if [ "$alive" = "ALIVE" ]; then
            return 0
        fi
        sleep 1
        i=$((i + 1))
    done
    echo "  server 日志尾部:"
    ssh_exec "${server_ip}" "tail -10 /tmp/urma_sweep_server_${server_ip}.log 2>/dev/null" | sed 's/^/  | /'
    return 1
}

print_intro() {
    echo "============================================================"
    echo " URMA 消息尺寸扫描 —— 定位 46 vs 38.5 GiB/s 的差距在哪一层"
    echo "============================================================"
    echo ""
    echo "[本脚本怎么测（与 check_urma.sh 完全同一方式）]"
    echo "  server: $URMA_PERF write_bw -d ${DEVICE} -s <SIZE> -I ${INLINE_SIZE} --ctp"
    echo "  client: 同命令 + -S <server_ip>"
    echo "  流程: ssh 在 server 节点 nohup 启动 server → pgrep 确认就绪 →"
    echo "        ssh 在 client 节点跑 client → 取数值行的 BW average (MB/s)"
    echo "        （提取逻辑与 check_urma.sh 的 awk 相同，逐字符一致）"
    echo "  除 -s 外不改 urma_perftest 任何参数 → 64KB 行可直接对照 47148 的历史值"
    echo ""
    echo "[write_bw 在测什么]"
    echo "  client 把本端 buffer RDMA-write 进 server 预注册的内存，"
    echo "  测的是 URMA 数据管道的裸单向带宽（无 SPDK/NVMe 层）。"
    echo "  双向都测：SPDK 的 WRITE(拉数) 与 READ(推数) 两个方向都走这条管道。"
    echo ""
    echo "[46 GiB/s 这个数是怎么来的]"
    echo "  check_urma.sh 之前在 node2↔node3 上测得:"
    echo "    $URMA_PERF write_bw -d udmac0d1e2 -s 65536 -I 128 --ctp"
    echo "    → BW average = ${BASELINE_MBPS} MB/s (≈$((BASELINE_MBPS/1024)) GiB/s)"
    echo "  注意那是 64KB 消息; SPDK v6 是 4MB 单条 pull, 实测 ${SPDK_GIBPS} GiB/s。"
    echo ""
    echo "[结果怎么读]"
    echo "  - 若 -s 4MB 行仍在 ~$((BASELINE_MBPS/1024)) GiB/s:"
    echo "      46 的带宽 UMDK 给得出来 → 差距在 SPDK 栈内 → 下一步 SPDK_URMA_TRACE=1"
    echo "  - 若 -s 4MB 行掉到 ~${SPDK_GIBPS} GiB/s:"
    echo "      UMDK 对大消息有墙 → 出路是 chunked pull(4MB 拆小 WR) 或 bonding"
    echo "  - 若 -s 4MB 行直接报错:"
    echo "      UMDK 单条消息有尺寸上限, 这本身就是关键发现"
    echo ""
    echo "[配置] DEVICE=${DEVICE}  SIZES=\"${SIZES}\""
    echo "        client=${CLIENT_NAME}(${CLIENT_NODE_IP})  server=${SERVER_NAME}(${SERVER_NODE_IP})"
}

precheck() {
    log_info "预检 SSH 连通性..."
    local ip
    for ip in "${SERVER_NODE_IP}" "${CLIENT_NODE_IP}"; do
        if ssh_exec "${ip}" "echo ok" >/dev/null 2>&1; then
            log_ok "${ip}: SSH 可达"
        else
            log_fail "${ip}: SSH 不可达 (检查网络/sshd/密码)"
            exit 1
        fi
    done
    for ip in "${SERVER_NODE_IP}" "${CLIENT_NODE_IP}"; do
        if ssh_exec "${ip}" "command -v ${URMA_PERF} >/dev/null 2>&1"; then
            log_ok "${ip}: ${URMA_PERF} 在 PATH 中"
        else
            log_fail "${ip}: 找不到 ${URMA_PERF}（用 URMA_PERF=/full/path 覆盖）"
            exit 1
        fi
    done
}

run_sweep() {
    local client_ip="$1" server_ip="$2" cname="$3" sname="$4"
    local size srv_pid output ret bw gib logf

    for size in ${SIZES}; do
        echo ""
        echo "----------------------------------------------------------------"
        echo "[RUN] ${cname}(${client_ip}) → ${sname}(${server_ip})  消息 $(size_label ${size}) (${size} B)"

        cleanup_node "${server_ip}"

        # 1. server 端后台启动
        srv_pid=$(ssh_exec_bg "${server_ip}" "${URMA_PERF} write_bw -d ${DEVICE} -s ${size} -I ${INLINE_SIZE} --ctp")
        if ! echo "${srv_pid}" | grep -q '^[0-9]\+$'; then
            log_fail "server 在 ${sname} 启动失败"
            record "${cname}->${sname}" "${size}" "SRV_FAIL"
            continue
        fi

        # 2. 等 server 就绪
        if ! wait_server_ready "${server_ip}"; then
            log_fail "server 就绪超时"
            record "${cname}->${sname}" "${size}" "SRV_TIMEOUT"
            cleanup_node "${server_ip}"
            continue
        fi

        # 3. client 端（timeout 包裹，同 check_urma.sh）
        logf="${LOG_DIR}/${cname}_to_${sname}_${size}.log"
        output=$(ssh_exec "${client_ip}" "timeout ${TIMEOUT} ${URMA_PERF} write_bw -d ${DEVICE} -s ${size} -I ${INLINE_SIZE} --ctp -S ${server_ip}" 2>&1)
        ret=$?
        echo "${output}" > "${logf}"
        cleanup_node "${server_ip}"

        if [ $ret -eq 124 ]; then
            log_fail "client 超时(${TIMEOUT}s)，输出已存 ${logf}"
            record "${cname}->${sname}" "${size}" "TIMEOUT"
            continue
        elif [ $ret -ne 0 ]; then
            log_fail "client 退出码=${ret}，输出已存 ${logf}"
            echo "${output}" | tail -5 | sed 's/^/  | /'
            record "${cname}->${sname}" "${size}" "ERROR"
            continue
        fi

        # 4. 原始输出全量展示 + 提取（与 check_urma.sh 相同的 awk）
        echo "${output}" | sed 's/^/  | /'
        bw=$(echo "${output}" | awk '/^[[:space:]]*[0-9]/{print $4}' | tail -1)
        if echo "${bw}" | grep -q '^[0-9.]\+$'; then
            gib=$(awk "BEGIN{printf \"%.1f\", ${bw}/1024}")
            echo "  ==> BW average = ${bw} MB/s (${gib} GiB/s)"
            record "${cname}->${sname}" "${size}" "${bw}"
        else
            log_fail "未提取到 BW（原始输出见 ${logf}）"
            record "${cname}->${sname}" "${size}" "NO_BW"
        fi
    done
}

print_summary() {
    echo ""
    echo "============================================================"
    echo " 汇总 (BW average, 提取方式与 check_urma.sh 一致)"
    echo "------------------------------------------------------------"
    printf "%-16s %8s %12s %9s %10s\n" "方向" "消息" "MB/s" "GiB/s" "vs基线"
    local dir size bw lbl gib ratio
    while IFS='|' read -r dir size bw; do
        lbl=$(size_label "${size}")
        if echo "${bw}" | grep -q '^[0-9.]\+$'; then
            gib=$(awk "BEGIN{printf \"%.1f\", ${bw}/1024}")
            ratio=$(awk "BEGIN{printf \"%.0f\", ${bw}*100/${BASELINE_MBPS}}")
            printf "%-16s %8s %12s %9s %9s%%\n" "${dir}" "${lbl}" "${bw}" "${gib}" "${ratio}"
        else
            printf "%-16s %8s %12s %9s %10s\n" "${dir}" "${lbl}" "${bw}" "-" "-"
        fi
    done < "${RESULT_FILE}"
    echo "------------------------------------------------------------"
    echo " 基线: ${BASELINE_MBPS} MB/s (≈$((BASELINE_MBPS/1024)) GiB/s) = check_urma.sh -s 65536 历史值"
    echo " SPDK v6 对照: 9SSD 4MB pull 实测 ${SPDK_GIBPS} GiB/s (node3 initiator → node2 target)"
    echo ""
    echo " 判读:"
    echo "   4MB 行 ≈ 基线        → 差距在 SPDK 栈内 (下一步 SPDK_URMA_TRACE=1 拆解)"
    echo "   4MB 行 ≈ ${SPDK_GIBPS} GiB/s    → UMDK 大消息墙 (chunked pull / bonding 抬天花板)"
    echo "   4MB 行报错           → UMDK 单条消息尺寸上限, 本身即关键发现"
    echo "============================================================"
    echo " 各次运行的完整原始输出: ${LOG_DIR}/"
}

trap_cleanup() {
    echo ""
    echo "[WARN] 收到中断信号，正在清理..."
    cleanup_node "${SERVER_NODE_IP}"
    [ "${ONE_WAY}" = "1" ] || cleanup_node "${CLIENT_NODE_IP}"
    rm -f "${RESULT_FILE}"
    exit 130
}
trap trap_cleanup INT TERM

# ======================== 主流程 ========================
main() {
    print_intro
    check_sshpass
    mkdir -p "${LOG_DIR}"
    > "${RESULT_FILE}"
    precheck

    run_sweep "${CLIENT_NODE_IP}" "${SERVER_NODE_IP}" "${CLIENT_NAME}" "${SERVER_NAME}"
    if [ "${ONE_WAY}" != "1" ]; then
        run_sweep "${SERVER_NODE_IP}" "${CLIENT_NODE_IP}" "${SERVER_NAME}" "${CLIENT_NAME}"
    fi

    print_summary
    rm -f "${RESULT_FILE}"
}

main "$@"
