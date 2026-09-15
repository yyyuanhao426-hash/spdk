#!/bin/bash
# =============================================================================
# URMA NUMA 体检 (urma_numa_check.sh, 只读)
# 功能：对 target/initiator 两个节点做只读 NUMA 体检，为
#       "SPDK vs 裸 urma_perftest 的 ~10% 差距 = 内存位置/NUMA" 假设收集证据。
#       不改任何配置、不重启任何进程、不做压测。
#
# 每个节点收集 8 组信息：
#   [1] CPU/NUMA 拓扑 (lscpu + numactl -H, 含 node 间 distance)
#   [2] 大页配置 (/proc/meminfo + per-node total/free + cmdline + THP + hugetlbfs 挂载)
#   [3] URMA 设备定位 (DEVICE→BDF 推断 + 全 PCI 清单里网卡/NVMe/URMA 驱动的 numa)
#   [4] 数据面网口定位 (141.61.84.x / 192.168.84.x 网卡的 numa_node + local_cpulist)
#   [5] nvmf_tgt 进程 (若在跑): cmdline / CPU 亲和 / numastat / numa_maps 大页落点分布 / 线程核分布
#   [6] urma_perf 进程 (若在跑): 同上
#   [7] 模块与内核日志线索 (lsmod + dmesg 里大页/分配失败)
#   [8] 内存水位 (free -h)
#
# 判读要点见脚本末尾 SUMMARY。核心两条：
#   - nvmf_tgt 的大页映射落在哪个 NUMA node？与 URMA 设备/网卡的 numa_node 同侧还是异侧？
#   - nvmf_tgt 的 reactor 核是否落在 NIC 的 local_cpulist 内？
#
# 若体检指向 "大页/进程落在远端 node"，再手动跑 membind 对照（裸工具，一次一端，见 URMA 排查记录）：
#   server(node2): numactl --membind=<NIC本地node> urma_perftest write_bw -d <dev> -s 4194304 -I 128 --ctp
#                  numactl --membind=<远端node>   同上，两者差值即跨 node 访存代价
#
# 用法：
#   ./urma_numa_check.sh                        # 两节点都查
#   CLIENT_NODE_IP=x SERVER_NODE_IP=y ./...     # 换卡后 SSH IP 变了就覆盖
#   DEVICE=bonding_dev_0 ./urma_numa_check.sh   # 换设备名
# 前提：执行机对两个节点已配好 SSH 密钥免密登录（本脚本用普通 ssh，不用 sshpass）。
# =============================================================================

set -u

# ======================== 配置区 ========================
CLIENT_NODE_IP="${CLIENT_NODE_IP:-141.61.84.149}"    # node3（initiator 侧）
SERVER_NODE_IP="${SERVER_NODE_IP:-141.61.84.247}"    # node2（target 侧，nvmf_tgt 在这）
CLIENT_NAME="${CLIENT_NAME:-node3}"
SERVER_NAME="${SERVER_NAME:-node2}"
DEVICE="${DEVICE:-udmac0d1e2}"                       # 换卡后设备名可能变，看 [3] 的 PCI 清单
SSH_PORT="${SSH_PORT:-22}"
SSH_USER="${SSH_USER:-root}"
LOG_DIR="/tmp/urma_numa_check_logs"

# ======================== 函数区 ========================
log_info() { echo "[INFO] $*"; }
log_ok()   { echo "[OK]   $*"; }
log_fail() { echo "[FAIL] $*"; }

ssh_exec() {
    local host="$1"
    local cmd="$2"
    local err_file="/tmp/urma_numa_ssh_err_$$"
    local output
    output=$(ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 \
        -p "${SSH_PORT}" "${SSH_USER}@${host}" "${cmd}" 2>"${err_file}")
    local ret=$?
    rm -f "${err_file}"
    echo "$output"
    return $ret
}

# ---- 远程诊断函数（declare -f 原样发到对端 bash 执行，不经本地展开）----
node_diag() {
    echo "#### [1] CPU / NUMA 拓扑"
    lscpu 2>/dev/null | grep -iE '^(Model name|Socket|NUMA|CPU\(s\)|On-line|Thread|Core)'
    echo ""
    if command -v numactl >/dev/null 2>&1; then
        numactl -H 2>/dev/null
    else
        echo "(numactl 未安装，用 /sys 兜底)"
        for dd in /sys/devices/system/node/node[0-9]*; do
            [ -f "$dd/distance" ] && echo "$(basename "$dd") distance: $(cat "$dd/distance")"
        done
    fi

    echo ""
    echo "#### [2] 大页配置"
    grep -iE 'HugePages|Hugepagesize' /proc/meminfo 2>/dev/null
    echo "vm.nr_hugepages = $(sysctl -n vm.nr_hugepages 2>/dev/null)"
    echo "THP enabled: $(grep -o '\[[a-z]*\]' /sys/kernel/mm/transparent_hugepage/enabled 2>/dev/null)"
    echo "cmdline: $(cat /proc/cmdline 2>/dev/null)"
    grep hugetlb /proc/mounts 2>/dev/null | sed 's/^/mount: /'
    echo "-- per-node 大页 (total/free):"
    for f in /sys/devices/system/node/node[0-9]*/hugepages/hugepages-*; do
        [ -d "$f" ] || continue
        printf "  %s total=%s free=%s\n" "$f" "$(cat "$f/total_hugepages" 2>/dev/null)" "$(cat "$f/free_hugepages" 2>/dev/null)"
    done

    echo ""
    echo "#### [3] URMA 设备 (DEVICE=${DEVICE})"
    # udmac0d1e2 → 猜 0000:0d:1e.2（仅推断，目录不存在就靠下面的 PCI 全清单）
    local bdf pd
    bdf=$(echo "${DEVICE}" | sed -n 's/^udmac\([0-9a-fA-F][0-9a-fA-F]\)\([0-9a-fA-F][0-9a-fA-F]\)\([0-9a-fA-F]\)$/0000:\1:\2.\3/p')
    if [ -n "$bdf" ] && [ -d "/sys/bus/pci/devices/$bdf" ]; then
        pd="/sys/bus/pci/devices/$bdf"
        echo "  设备名→BDF 推断命中: ${DEVICE} → ${bdf}"
        echo "    numa_node     = $(cat "$pd/numa_node" 2>/dev/null)"
        echo "    local_cpulist = $(cat "$pd/local_cpulist" 2>/dev/null)"
        echo "    vendor/device = $(cat "$pd/vendor" 2>/dev/null) $(cat "$pd/device" 2>/dev/null)"
        local drv="-"
        [ -e "$pd/driver" ] && drv=$(basename "$(readlink -f "$pd/driver")")
        echo "    driver        = ${drv}"
        echo "    link          = $(cat "$pd/current_speed" 2>/dev/null) $(cat "$pd/current_width" 2>/dev/null)"
    else
        echo "  设备名→BDF 推断未命中 (bdf=${bdf:-空})，用下面的 PCI 清单人工找"
    fi
    echo "-- PCI 清单 (网卡 0x02 / NVMe 0x0108 / urma|udma 驱动):"
    for pd in /sys/bus/pci/devices/*; do
        [ -d "$pd" ] || continue
        local cls drv numa keep
        cls=$(cat "$pd/class" 2>/dev/null)
        drv="-"
        [ -e "$pd/driver" ] && drv=$(basename "$(readlink -f "$pd/driver")")
        numa=$(cat "$pd/numa_node" 2>/dev/null)
        keep=""
        case "$drv" in *urma*|*udma*) keep="y";; esac
        case "$cls" in 0x02*|0x0108*) keep="y";; esac
        [ -n "$keep" ] || continue
        printf "  %-13s numa=%-3s cls=%-7s drv=%s\n" "$(basename "$pd")" "$numa" "$cls" "$drv"
    done | sort

    echo ""
    echo "#### [4] 数据面网口 (141.61.84.x / 192.168.84.x)"
    if command -v ip >/dev/null 2>&1; then
        ip -o addr show 2>/dev/null | awk '$4 ~ /^(141\.61\.84|192\.168\.84)\./ {print $2, $4}' \
        | while read -r dev addr; do
            local pd drv
            pd="/sys/class/net/$dev/device"
            drv="-"
            [ -e "$pd/driver" ] && drv=$(basename "$(readlink -f "$pd/driver")")
            echo "  ${addr} → ${dev}: numa_node=$(cat "$pd/numa_node" 2>/dev/null) local_cpulist=$(cat "$pd/local_cpulist" 2>/dev/null) driver=${drv}"
        done
    else
        echo "  (无 ip 命令，跳过网口定位)"
    fi

    echo ""
    echo "#### [5] nvmf_tgt 进程落点"
    proc_diag "nvmf_tgt" "nvmf_tgt"
    echo ""
    echo "#### [6] urma_perf / urma_perftest 进程落点"
    proc_diag "urma_perf" "urma_perf"

    echo ""
    echo "#### [7] 模块与内核日志线索"
    lsmod 2>/dev/null | grep -iE 'urma|udma|nvme' | head -10
    dmesg 2>/dev/null | grep -iE 'hugepage|alloc.*fail|out of memory|numa' | tail -8

    echo ""
    echo "#### [8] 内存水位"
    free -h 2>/dev/null
}

proc_diag() {
    # $1 = pgrep -f 模式, $2 = 显示名
    local pat="$1" label="$2"
    local pid
    pid=$(pgrep -f "${pat}" 2>/dev/null | head -1)
    if [ -z "$pid" ]; then
        echo "  (${label} 未在运行 —— 起来之后再跑一遍本脚本才看得到内存落点)"
        return 0
    fi
    echo "  pid=${pid}"
    echo "  cmdline: $(tr '\0' ' ' < "/proc/${pid}/cmdline" 2>/dev/null)"
    grep -E '^(Cpus_allowed_list|Mems_allowed_list)' "/proc/${pid}/status" 2>/dev/null | sed 's/^/  /'
    echo "  线程数: $(ls "/proc/${pid}/task" 2>/dev/null | wc -l)"
    if command -v numastat >/dev/null 2>&1; then
        echo "  -- numastat -p ${pid} (KB):"
        numastat -p "${pid}" 2>/dev/null | sed 's/^/    /'
    else
        echo "  (numastat 未安装，用 numa_maps 兜底)"
    fi
    echo "  -- numa_maps 汇总 (huge 行 = 大页映射的落点分布，按页数):"
    awk '{
        ishuge = ($0 ~ /huge/)
        for (i = 1; i <= NF; i++) {
            if ($i ~ /^N[0-9]+=/) {
                split($i, a, "=")
                n = substr(a[1], 2) + 0
                v = a[2] + 0
                P[n] += v
                if (ishuge) H[n] += v
            }
        }
    } END {
        printf "    anon:"
        for (n = 0; n < 8; n++) if (n in P) printf " N%d=%d", n, P[n]
        printf "\n    huge:"
        any = 0
        for (n = 0; n < 8; n++) if (n in H) { any = 1; printf " N%d=%d", n, H[n] }
        if (!any) printf " (无大页映射)"
        printf "\n"
    }' "/proc/${pid}/numa_maps" 2>/dev/null
    echo "  -- 线程核分布 (psr):"
    ps -eLo pid,psr,comm 2>/dev/null | awk -v p="${pid}" '$1==p {print $2}' \
        | sort -n | uniq -c | sort -rn \
        | awk '{printf "    core %-4s x%s\n", $2, $1}' | head -30
}

# ---- 包装与汇总 ----
print_intro() {
    echo "============================================================"
    echo " URMA NUMA 体检 —— 只读收集，为 9-10% 差距的内存位置假设找证据"
    echo "============================================================"
    echo ""
    echo "[收集什么] 两节点的 NUMA 拓扑/大页配置/URMA 设备与网卡的 numa 归属/"
    echo "           nvmf_tgt、urma_perf 进程的大页落点与核分布"
    echo "[判读核心] nvmf_tgt 大页落在哪个 node？是否与 URMA 设备同侧？"
    echo "           reactor 核是否在 NIC 的 local_cpulist 内？"
    echo "[配置] DEVICE=${DEVICE}  ssh_user=${SSH_USER}"
    echo "       target=${SERVER_NAME}(${SERVER_NODE_IP})  initiator=${CLIENT_NAME}(${CLIENT_NODE_IP})"
}

precheck() {
    log_info "预检 SSH 连通性..."
    local ip
    for ip in "${SERVER_NODE_IP}" "${CLIENT_NODE_IP}"; do
        if ssh_exec "${ip}" "echo ok" >/dev/null 2>&1; then
            log_ok "${ip}: SSH 可达"
        else
            log_fail "${ip}: SSH 不可达 (检查网络/sshd/密钥认证)"
            exit 1
        fi
    done
}

run_diag() {
    local host="$1" name="$2"
    echo ""
    echo "============================================================"
    echo " ${name} (${host}) NUMA 体检"
    echo "============================================================"
    {
        printf 'DEVICE="%s"\n' "${DEVICE}"
        declare -f node_diag proc_diag
        echo "node_diag"
    } | ssh -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 \
        -p "${SSH_PORT}" "${SSH_USER}@${host}" "timeout 120 bash -s" 2>&1
}

print_summary() {
    echo ""
    echo "============================================================"
    echo " 判读要点（对照两节点的输出）"
    echo "------------------------------------------------------------"
    echo " 1. [5] nvmf_tgt 的 numa_maps huge 行: 大页落在 N0 还是 N1？"
    echo "    与 [3] URMA 设备 / [4] 网卡的 numa_node 对照:"
    echo "    同侧 = 内存本地(好)；异侧 = 跨 node 访存(就是我们要找的 9-10%)"
    echo " 2. [5] 线程核分布 (psr) 是否落在 [3] URMA 设备的 local_cpulist 内"
    echo " 3. [2] per-node 空闲大页: nvmf_tgt -s 申请量是否吃光了某个 node 的份额"
    echo " 4. [1] numactl -H 的 distance: 跨 node 代价 (一般 21 vs 10)"
    echo " 5. 若 [1] 只有一个 NUMA node (单路机器) → NUMA 假设直接出局，差距另有原因"
    echo " 6. [6] urma_perf 若在跑, 同样对照 initiator 侧"
    echo "------------------------------------------------------------"
    echo " 体检指向 '落在远端 node' 时, 按脚本头部注释跑 numactl --membind 裸工具对照"
    echo "============================================================"
}

# ======================== 主流程 ========================
main() {
    print_intro
    mkdir -p "${LOG_DIR}"
    precheck

    run_diag "${SERVER_NODE_IP}" "${SERVER_NAME}" | tee "${LOG_DIR}/${SERVER_NAME}_numa.txt"
    run_diag "${CLIENT_NODE_IP}" "${CLIENT_NAME}" | tee "${LOG_DIR}/${CLIENT_NAME}_numa.txt"

    print_summary
    echo ""
    echo " 各节点完整输出已存: ${LOG_DIR}/"
}

main "$@"
