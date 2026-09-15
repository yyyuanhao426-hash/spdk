#!/bin/bash
# =============================================================================
# URMA NUMA 体检 (urma_numa_check.sh, 只读)
# 功能：自动发现集群里所有 node 机器（/etc/hosts 的 node* + 种子 IP，可覆盖），
#       逐台只读收集：NUMA 拓扑 / 设备(NIC·NVMe·URMA)落点 / 大页 per-node 分布 /
#       nvmf_tgt·urma_perf 进程的内存与核落点，最后自动生成一行结论：
#         进程内存落在哪个 NUMA node vs 它的设备(NIC/盘/URMA卡)在哪个 node
#         → ✓ 同侧 / ✗ 错位
#       不改配置、不重启进程、不压测。
#
# 用法：
#   ./urma_numa_check.sh                                # 自动发现所有 node* 机器
#   NODES="node2=141.61.84.245 node3=141.61.84.149" ./urma_numa_check.sh
#   NODES="141.61.84.245 141.61.84.149" ./urma_numa_check.sh   # 名字取自对端 hostname
#   SEEDS="ip1 ip2" ./urma_numa_check.sh                # 自动发现的种子 IP
#   SSH_PORT=2222 SSH_USER=root ./urma_numa_check.sh
# 前提：执行机对各节点已配好 SSH 密钥免密登录（不用 sshpass）。
# =============================================================================

set -u

# ======================== 配置区 ========================
SEEDS="${SEEDS:-141.61.84.245 141.61.84.247 141.61.84.149 141.61.84.151}"
NODES="${NODES:-}"                                   # 留空 = 自动发现
SSH_PORT="${SSH_PORT:-22}"
SSH_USER="${SSH_USER:-root}"
TIMEOUT="${TIMEOUT:-180}"                            # 单台收集超时(秒)
LOG_DIR="/tmp/urma_numa_check_logs"
SSH_OPTS="-o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -p ${SSH_PORT}"

# ======================== 函数区 ========================
log_info() { echo "[INFO] $*"; }
log_ok()   { echo "[OK]   $*"; }
log_fail() { echo "[FAIL] $*"; }

ssh_exec() {  # $1=ip  $2=cmd
    ssh $SSH_OPTS "${SSH_USER}@${1}" "${2}" 2>/dev/null
}

# ---- 节点发现 ----
declare -A NODE_NAME_BY_IP NODE_IP_BY_NAME

add_node() {  # $1=ip  $2=name(可选，缺省问对端 hostname)
    local ip="$1" name="${2:-}"
    [ -n "$ip" ] || return 0
    case "$ip" in *[!A-Za-z0-9.:_-]*) return 0 ;; esac
    if [ -z "$name" ]; then
        name=$(ssh_exec "$ip" "hostname -s" | head -1)
        [ -n "$name" ] || name="$ip"
    fi
    local prev="${NODE_IP_BY_NAME[$name]:-}"
    if [ -n "$prev" ] && [ "$prev" != "$ip" ]; then return 0; fi   # 同名不同 IP，丢弃
    NODE_NAME_BY_IP["$ip"]="$name"
    NODE_IP_BY_NAME["$name"]="$ip"
}

discover_nodes() {
    local ip out l name iip
    if [ -n "$NODES" ]; then
        for l in $NODES; do
            case "$l" in *=*) add_node "${l#*=}" "${l%%=*}" ;; *) add_node "$l" ;; esac
        done
        return 0
    fi
    log_info "自动发现节点 (种子: ${SEEDS})..."
    for ip in $SEEDS; do
        if ssh_exec "$ip" "echo ok" | grep -q ok; then
            add_node "$ip"
            # /etc/hosts 里的 node* → name=ip
            out=$(ssh_exec "$ip" "grep -E 'node[0-9]+' /etc/hosts 2>/dev/null | awk '{ip=\$1; for(i=2;i<=NF;i++) if (\$i ~ /^node[0-9]+\$/) print \$i\"=\"\$1}' | sort -u")
            for l in $out; do
                name="${l%%=*}"; iip="${l#*=}"
                add_node "$iip" "$name"
            done
            # 该机自身（免得 /etc/hosts 没写自己）
            out=$(ssh_exec "$ip" "hostname -I 2>/dev/null | awk '{print \$1}'")
            [ -n "$out" ] && add_node "$out"
        else
            log_fail "种子 ${ip} 不可达，跳过"
        fi
    done
}

node_pairs() {  # 输出 "name|ip"，按名字排序
    local ip
    for ip in "${!NODE_NAME_BY_IP[@]}"; do
        echo "${NODE_NAME_BY_IP[$ip]}|${ip}"
    done | sort -t'|' -k1,1V
}

# ---- 远程收集（declare -f 原样发到对端 bash 执行）----
node_diag() {
    echo "#### [1] NUMA 拓扑"
    local nn cpus model d n cp seg atseg hp sz t f
    nn=$(ls -d /sys/devices/system/node/node[0-9]* 2>/dev/null | wc -l)
    cpus=$(nproc 2>/dev/null)
    model=$(grep -m1 -i 'model name' /proc/cpuinfo 2>/dev/null | cut -d: -f2 | sed 's/^ *//')
    echo "  ${nn} 个 NUMA node / ${cpus} cpu / ${model}"
    echo "@TOPO|nodes=${nn}|cpus=${cpus}|model=${model}"
    for d in /sys/devices/system/node/node[0-9]*; do
        [ -d "$d" ] || continue
        n=$(basename "$d"); cp=$(cat "$d/cpulist" 2>/dev/null)
        seg=""; atseg=""
        for hp in "$d"/hugepages/hugepages-*; do
            [ -d "$hp" ] || continue
            sz=$(basename "$hp" | sed 's/hugepages-//;s/kB$/k/')
            t=$(cat "$hp/total_hugepages" 2>/dev/null)
            f=$(cat "$hp/free_hugepages" 2>/dev/null)
            seg="${seg}${sz}: total=${t:-?} free=${f:-?}  "
            atseg="${atseg}|${sz}_total=${t:-?}|${sz}_free=${f:-?}"
        done
        echo "  ${n}: cpus=${cp}  ${seg}"
        echo "@NUMA|${n}|cpus=${cp}${atseg}"
    done
    echo "  distance(node0): $(cat /sys/devices/system/node/node0/distance 2>/dev/null | tr -s ' \n' ' ')"
    echo "@DIST|$(cat /sys/devices/system/node/node0/distance 2>/dev/null | tr -s ' \n' ' ')"

    echo ""
    echo "#### [2] 设备 → NUMA (NIC / NVMe / urma|udma 驱动)"
    local pd cls drv numa keep lcp
    for pd in /sys/bus/pci/devices/*; do
        [ -d "$pd" ] || continue
        cls=$(cat "$pd/class" 2>/dev/null); drv="-"
        [ -e "$pd/driver" ] && drv=$(basename "$(readlink -f "$pd/driver")")
        numa=$(cat "$pd/numa_node" 2>/dev/null)
        keep=""
        case "$drv" in *urma*|*udma*) keep="y" ;; esac
        case "$cls" in 0x02*|0x0108*) keep="y" ;; esac
        [ -n "$keep" ] || continue
        lcp=$(cat "$pd/local_cpulist" 2>/dev/null)
        printf "  %-13s numa=%-3s drv=%-12s cls=%-8s cpus=%s\n" "$(basename "$pd")" "$numa" "$drv" "$cls" "$lcp"
        echo "@DEV|$(basename "$pd")|numa=${numa}|drv=${drv}|cls=${cls}|cpus=${lcp}"
    done | sort

    echo ""
    echo "#### [3] 数据面网口 (141.61.84.x / 192.168.84.x)"
    ip -o addr show 2>/dev/null | awk '$4 ~ /^(141\.61\.84|192\.168\.84)\./ {print $2, $4}' \
    | while read -r dev addr; do
        pd="/sys/class/net/$dev/device"
        drv="-"; [ -e "$pd/driver" ] && drv=$(basename "$(readlink -f "$pd/driver")")
        numa=$(cat "$pd/numa_node" 2>/dev/null); lcp=$(cat "$pd/local_cpulist" 2>/dev/null)
        echo "  ${addr} → ${dev}: numa=${numa} cpus=${lcp} drv=${drv}"
        echo "@NET|${addr}|dev=${dev}|numa=${numa}|drv=${drv}|cpus=${lcp}"
    done

    echo ""
    echo "#### [4] 进程落点"
    proc_diag "nvmf_tgt" "nvmf_tgt"
    proc_diag "urma_perf" "urma_perf"
    pgrep -f 'nvmf_tgt|urma_perf' >/dev/null 2>&1 || \
        echo "  (nvmf_tgt / urma_perf 都未在跑 —— 起来后再跑一遍本脚本才看得到内存落点)"

    echo ""
    echo "#### [5] 线索"
    lsmod 2>/dev/null | grep -iE 'urma|udma' | head -5
    dmesg 2>/dev/null | grep -iE 'hugepage|alloc.*fail|out of memory' | tail -3
}

proc_diag() {  # $1=pgrep 模式  $2=显示名
    local pat="$1" label="$2" pid
    for pid in $(pgrep -f "$pat" 2>/dev/null | sort -n); do
        local cmd allowed mems nthr huge anon
        cmd=$(tr '\0' ' ' < "/proc/${pid}/cmdline" 2>/dev/null)
        allowed=$(awk '/^Cpus_allowed_list/{print $2}' "/proc/${pid}/status" 2>/dev/null)
        mems=$(awk '/^Mems_allowed_list/{print $2}' "/proc/${pid}/status" 2>/dev/null)
        nthr=$(ls "/proc/${pid}/task" 2>/dev/null | wc -l)
        huge=""; anon=""
        eval "$(awk '{
            ishuge = ($0 ~ /huge/)
            for (i = 1; i <= NF; i++) {
                if ($i ~ /^N[0-9]+=/) {
                    split($i, a, "=")
                    n = substr(a[1], 2) + 0; v = a[2] + 0
                    P[n] += v; if (ishuge) H[n] += v
                }
            }
        } END {
            printf "huge=\""; f=1
            for (n in H) { if (!f) printf ","; printf "N%d:%d", n, H[n]; f=0 }
            printf "\" anon=\""; f=1
            for (n in P) { if (!f) printf ","; printf "N%d:%d", n, P[n]-H[n]; f=0 }
            print "\""
        }' "/proc/${pid}/numa_maps" 2>/dev/null)"
        huge="${huge:-none}"; anon="${anon:-none}"
        echo "  ${label} pid=${pid} threads=${nthr} Cpus_allowed=${allowed:-?} Mems_allowed=${mems:-?}"
        echo "    cmdline: ${cmd}"
        echo "    大页落点: ${huge}   匿名页落点: ${anon}"
        echo "@PROC|${label}|${pid}|allowed=${allowed:-?}|mems=${mems:-?}|threads=${nthr}|huge=${huge}|anon=${anon}"
        ps -eLo pid,psr,comm 2>/dev/null | awk -v p="${pid}" '$1==p {print $2}' \
            | sort -n | uniq -c | sort -rn | head -5 \
            | awk '{printf "    core %-4s x%s\n", $2, $1}'
    done
}

# ---- 本地汇总 ----
declare -A R_TOPO R_DIST R_NUMA R_DEV R_NET R_PROC

parse_at() {  # $1=节点名  $2=@行
    local node="$1" line="$2" key
    case "$line" in
        "@TOPO|"*) R_TOPO["$node"]="${line#@TOPO|}" ;;
        "@DIST|"*) R_DIST["$node"]="${line#@DIST|}" ;;
        "@NUMA|"*) key="${line#@NUMA|}"; R_NUMA["$node|${key%%|*}"]="${key#*|}" ;;
        "@DEV|"*)  key="${line#@DEV|}";  R_DEV["$node|${key%%|*}"]="${key#*|}" ;;
        "@NET|"*)  key="${line#@NET|}";  R_NET["$node|${key%%|*}"]="${key#*|}" ;;
        "@PROC|"*) key="${line#@PROC|}"; R_PROC["$node|${key%%|*}"]="${key#*|}" ;;
    esac
}

getf() {  # $1=a=1|b=2 形式串  $2=字段名 → 值
    local p
    for p in ${1//|/ }; do
        case "$p" in "$2="*) printf '%s' "${p#*=}"; return ;; esac
    done
}

dev_kind() {  # $1=drv $2=cls
    case "$1" in *urma*|*udma*) echo "URMA"; return ;; esac
    case "$2" in
        0x02*)   echo "NIC" ;;
        0x0108*) echo "NVMe" ;;
        *)       echo "?" ;;
    esac
}

run_diag() {  # $1=name $2=ip
    local name="$1" ip="$2" out
    echo ""
    echo "============================================================"
    echo " ${name} (${ip})"
    echo "============================================================"
    if ! out=$( { declare -f node_diag proc_diag; echo node_diag; } \
            | ssh $SSH_OPTS "${SSH_USER}@${ip}" "timeout ${TIMEOUT} bash -s" 2>&1 ); then
        echo "  ✗ SSH 执行失败（跳过，不影响其它节点）"
        return 1
    fi
    printf '%s\n' "$out" > "${LOG_DIR}/${name}_numa.txt"
    printf '%s\n' "$out" | grep -v '^@'          # 人看的
    while read -r l; do parse_at "$name" "$l"; done < <(printf '%s\n' "$out" | grep '^@')
}

print_summary() {
    local name ip key rest numa drv cls kind line seg
    local miss=0 any dn parts p nn vv total maxv hpct
    echo ""
    echo "============================================================"
    echo " 自动结论：进程内存落点 vs 设备落点"
    echo "============================================================"
    while IFS='|' read -r name ip; do
        echo ""
        echo "■ ${name} (${ip})"
        if [ -z "${R_TOPO[$name]:-}" ]; then
            echo "   ✗ 无数据 (SSH 失败/超时)"
            miss=$((miss+1)); continue
        fi
        echo "   拓扑: ${R_TOPO[$name]:-?}  distance: ${R_DIST[$name]:-?}"
        # per-node 大页空闲（哪个 node 有页可分）
        line=""
        for key in "${!R_NUMA[@]}"; do
            [ "${key%%|*}" = "$name" ] || continue
            rest="${R_NUMA[$key]}"
            numa="${key#*|}"
            vv=$(getf "$rest" "2048k_free")
            [ -z "$vv" ] && vv=$(getf "$rest" "1048576k_free")
            line="${line}${numa}=${vv:-?} "
        done
        echo "   大页free: ${line}"
        # 设备按 NUMA 归组
        local -A cnt=() dnode=()
        for key in "${!R_DEV[@]}"; do
            [ "${key%%|*}" = "$name" ] || continue
            rest="${R_DEV[$key]}"
            numa=$(getf "$rest" numa); drv=$(getf "$rest" drv); cls=$(getf "$rest" cls)
            kind=$(dev_kind "$drv" "$cls")
            case "$numa" in ""|-1) numa="?" ;; esac
            case "$numa" in "?") : ;; *) numa="N${numa}" ;; esac
            cnt["${numa}|${kind}"]=$(( ${cnt["${numa}|${kind}"]:-0} + 1 ))
            [ "$numa" != "?" ] && dnode["$numa"]=1
        done
        if [ ${#cnt[@]} -eq 0 ]; then
            echo "   设备: (未发现 NIC/NVMe/URMA 设备)"
        else
            line=""
            for numa in $(printf '%s\n' "${!cnt[@]}" | cut -d'|' -f1 | sort -uV); do
                seg=""
                for kind in URMA NIC NVMe "?"; do
                    c=${cnt["${numa}|${kind}"]:-0}
                    [ "$c" -gt 0 ] && seg="${seg}${kind}×${c} "
                done
                line="${line}${numa}: ${seg}| "
            done
            echo "   设备: ${line%| }"
        fi
        # 进程判定
        any=0
        for key in "${!R_PROC[@]}"; do
            [ "${key%%|*}" = "$name" ] || continue
            any=1
            local kp label pid
            kp="${key#*|}"; label="${kp%%|*}"; pid="${kp#*|}"
            rest="${R_PROC[$key]}"
            # 大页优先，无大页再看匿名页
            dist=$(getf "$rest" huge)
            if [ -z "$dist" ] || [ "$dist" = "none" ]; then dist=$(getf "$rest" anon); fi
            hpct=0; maxn=""
            if [ -n "$dist" ] && [ "$dist" != "none" ]; then
                total=0; maxv=0
                IFS=',' read -ra parts <<< "$dist"
                for p in "${parts[@]}"; do
                    nn="${p%%:*}"; vv="${p##*:}"
                    total=$((total+vv))
                    [ "$vv" -gt "$maxv" ] && { maxv=$vv; maxn=$nn; }
                done
                [ "$total" -gt 0 ] && hpct=$(( 100*maxv/total ))
            fi
            dn=$(for numa in "${!dnode[@]}"; do echo "$numa"; done | sort -V | paste -sd, -)
            if [ -z "$maxn" ]; then
                echo "   ${label} pid=${pid}: 无内存映射数据"
            elif [ -z "$dn" ]; then
                echo "   ${label} pid=${pid}: 内存 ${maxn} ${hpct}% (设备落点未知)"
            elif [[ ",${dn}," == *",${maxn},"* ]]; then
                local multi=""
                case "$dn" in *,*) multi=" (设备跨node)" ;; esac
                echo "   ${label} pid=${pid}: 内存 ${maxn} ${hpct}% vs 设备 {${dn}} → ✓ 同侧${multi}"
            else
                echo "   ${label} pid=${pid}: 内存 ${maxn} ${hpct}% vs 设备 {${dn// /,}} → ✗ 错位"
                miss=$((miss+1))
            fi
        done
        [ "$any" = 0 ] && echo "   进程: nvmf_tgt/urma_perf 未运行（只有设备落点）"
    done < <(node_pairs)
    echo ""
    if [ "$miss" -gt 0 ]; then
        echo "有 ✗ 项时的一步行动: 把进程核/内存挪到设备同侧再对比 ——"
        echo "  nvmf_tgt: 重启加 -m '<设备同侧核列表>';  裸工具对照: numactl --membind=<设备node> urma_perftest ..."
    fi
}

# ======================== 主流程 ========================
main() {
    mkdir -p "${LOG_DIR}"
    discover_nodes
    if [ "${#NODE_NAME_BY_IP[@]}" -eq 0 ]; then
        log_fail "没有任何可达节点 (检查 SEEDS / NODES / SSH 免密)"
        exit 1
    fi
    log_ok "参与体检 (${#NODE_NAME_BY_IP[@]} 台): $(node_pairs | tr '\n' ' ' | tr '|' '=')"

    local name ip
    while IFS='|' read -r name ip; do
        run_diag "$name" "$ip" || true
    done < <(node_pairs)

    print_summary
    echo ""
    echo "各节点原始输出: ${LOG_DIR}/"
}

main "$@"
