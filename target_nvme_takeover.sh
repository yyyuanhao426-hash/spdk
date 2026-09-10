#!/bin/bash
#===============================================================================
# target_nvme_takeover.sh — Target 节点 NVMe 接管（vfio-pci noiommu）+ nvmf_tgt + RPC 一键脚本
#
# 流程：
#   1. lsblk 展示系统盘分布，标出“绝对不能碰”的盘（root/home 挂载、LVM PV、md 成员）
#   2. 分析候选空闲盘；选定盘后【现查】其 BDF（nvmeXn1 每次开机重排，旧记录不可信）
#   3. lspci -nns 显示 vendor:device，用该 BDF 执行 driver_override → unbind → bind
#   4. 验证 vfio-pci 已接管 + 根分区仍是 rw（bind 错盘 = 根文件系统变 ro，实测过）
#   5. 环境检查：liburma 与 liburma_common 成套、hugepages（按 iobuf 池自动上调）、无残留 nvmf_tgt
#   6. 后台启动 ./build/bin/nvmf_tgt -m <mask>（计时为可选开关 -s 秒数；
#      给了 -I 时先以 --wait-for-rpc 启动，配好 iobuf 池再 framework_start_init）
#   7. RPC 就绪后自动配置：attach controller（每盘一个）→ create transport
#      → subsystem → ns（多盘默认多 namespace，-R 可合并成 raid0 单 namespace）
#      → listener（listener IP 自动探测，多网卡用 -i 显式指定）
#
# 用法：
#   ./target_nvme_takeover.sh                   # 只分析：列系统盘 + 空闲候选盘，不做任何变更
#   ./target_nvme_takeover.sh -d nvme3n1        # 接管 nvme3n1 → 后台启动 nvmf_tgt → 自动配 RPC
#   ./target_nvme_takeover.sh -d nvme3n1 -s 10  # 同上 + SPDK_URMA_TARGET_DUMP_SEC=10 计时
#   ./target_nvme_takeover.sh -d nvme3n1,nvme4n1    # 两块盘 → subsystem 里两个 namespace（nsid 1,2）
#   ./target_nvme_takeover.sh -d nvme3n1 -d nvme4n1 # 同上（-d 可重复，逗号分隔均可）
#   ./target_nvme_takeover.sh -d nvme3n1,nvme4n1 -R 128  # 两块盘合成 raid0（strip 128KB）→ 单 namespace
#   ./target_nvme_takeover.sh -d nvme3n1 -I 8192,8192,1024,4194304 -O 4194304
#       # 4MB 大 I/O：重配 iobuf 池（large=1024×4MB）+ URMA transport max_io_size=4MB
#       # 注意：initiator 侧也要 export SPDK_URMA_MAX_IO_SIZE=4194304，且 urma_perf -o ≤ 4MB
#
# 选项：
#   -d <盘名>    目标 NVMe 盘（如 nvme3n1）；可逗号分隔或重复 -d 接管多块盘；
#                不带则只做分析不接管
#   -R <strip>   把所有接管盘合成一个 raid0 bdev（单 namespace 聚合带宽），
#                <strip> 为 strip 大小 KB（如 128）；0 = 用 raid 模块默认 strip
#   -I <四元组>  iobuf 池 "小池数量,小池buf,大池数量,大池buf"（字节），
#                如 8192,8192,1024,4194304。启动时自动 hugepages 上调，
#                并以 --wait-for-rpc 启动后调 iobuf_set_options（仅 STARTUP 期可用）
#   -O <字节>    URMA transport max_io_size（2 的幂且 ≥8KB）。注意：urma 数据路径
#                要求 iovcnt==1，大 I/O 必须配合 -I 把 large_bufsize 配到 ≥ 此值
#   -s <秒>      打开 target 计时（SPDK_URMA_TARGET_DUMP_SEC，transport 创建时读取）
#   -i <IP>      listener 地址（默认自动探测本机第一个全局 IPv4；多网卡机器建议显式指定）
#   -m <掩码>    nvmf_tgt core mask（默认 0x3；盘多时可加宽，如 0xf）
#   -w <目录>    SPDK 源码根（默认从脚本所在目录向上找 build/bin/nvmf_tgt）
#   -L <目录>    URMA 库目录（默认 /home/yin/gdr/UMDK_netlab/lib，
#                必须同时含 liburma.so* 与 liburma_common.so*，缺一不可）
#   -N <设备名>  URMA 设备名（默认 udmac0d1e2）
#   -r           还原模式：把 vfio-pci 占用/游离的 NVMe 盘还原回 nvme 驱动后退出
#                （上次运行失败/中断后的补救）
#   -y           跳过交互确认（配合 -d 用于自动化）
#
# 接管前会自动检测上一次运行残留（vfio-pci 占用或游离的 NVMe 盘）并还原回
# nvme 驱动，确保本次是全新的 unbind + bind。
#
# 运行时产物：日志 /tmp/nvmf_tgt.log，PID /var/tmp/nvmf_tgt.pid
#             （停止：kill $(cat /var/tmp/nvmf_tgt.pid)；RPC 配置不持久化，重启后重跑本脚本）
#===============================================================================

set -u -o pipefail
# 防 env 污染：继承到 noglob 会让所有 * 失效（node1 实测：候选分析表打出字面
# "nvme*n"）。脚本自己把 globbing 打开，不依赖调用方 shell 的状态
set +f

HUGE_PAGES=2048

DISKS=""             # 逗号分隔累积（支持多次 -d），后面拆成数组
RAID0_STRIP=-1       # -1 = 不建 raid0；>=0 = 建 raid0（0 = 用 raid 模块默认 strip）
IOBUF_SPEC=""        # "小池数量,小池buf,大池数量,大池buf"
MAX_IO_SIZE=0        # URMA transport max_io_size（字节）
DUMP_SEC=""
COREMASK="0x3"
SPDK_DIR=""
LIBDIR="/home/yin/gdr/UMDK_netlab/lib"
DEVNAME="udmac0d1e2"
LISTEN_IP=""
RESTORE_ONLY=0
ASSUME_YES=0

NQN="nqn.2026-01.io.spdk:urma-gpu-test"
SUBSYS_SN="URMAGPU0001"
LISTEN_PORT=4420
RAID_NAME="URMA_RAID0"
TGT_LOG="/tmp/nvmf_tgt.log"
TGT_PIDFILE="/var/tmp/nvmf_tgt.pid"

usage() { awk 'NR==2{inhdr=1} inhdr && /^#/{sub(/^# ?/,""); print; next} inhdr{exit}' "$0"; exit 0; }

info() { echo "[INFO] $*"; }
ok()   { echo "[ OK ] $*"; }
warn() { echo "[WARN] $*"; }
abort() { echo "[ABORT] $*" >&2; exit 1; }
confirm() {
    [ "$ASSUME_YES" = 1 ] && return 0
    local a
    read -r -p "$1 [输入 yes 继续，其他=退出] " a
    [ "$a" = "yes" ]
}

while getopts "d:s:i:m:w:L:N:R:I:O:ryh" opt; do
    case $opt in
        d) DISKS="${DISKS:+$DISKS,}$OPTARG" ;;
        s) DUMP_SEC=$OPTARG ;;
        i) LISTEN_IP=$OPTARG ;;
        m) COREMASK=$OPTARG ;;
        w) SPDK_DIR=$OPTARG ;;
        L) LIBDIR=$OPTARG ;;
        N) DEVNAME=$OPTARG ;;
        R) RAID0_STRIP=$OPTARG ;;
        I) IOBUF_SPEC=$OPTARG ;;
        O) MAX_IO_SIZE=$OPTARG ;;
        r) RESTORE_ONLY=1 ;;
        y) ASSUME_YES=1 ;;
        h) usage ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))

[ "$(id -u)" -eq 0 ] || abort "必须以 root 运行（sysfs 绑定 + hugepages + 启动 nvmf_tgt）"

# 由盘名解析 BDF：readlink -f /sys/block/<盘> 的完整路径里取最后一个 PCI 地址
# （嵌套 PCIe 桥时最后一个才是端点）
get_bdf() {
    local p
    p=$(readlink -f "/sys/block/$1" 2>/dev/null) || return 1
    echo "$p" | grep -oE '[0-9a-fA-F]{4}:[0-9a-fA-F]{2}:[0-9a-fA-F]{2}\.[0-9a-fA-F]' | tail -n1
}

# 盘是否“禁碰”：返回原因（空 = 安全）
disk_reasons() {
    local d=$1 reasons=""
    local mnts
    mnts=$(lsblk -lno MOUNTPOINT "/dev/$d" 2>/dev/null | grep -v '^$')
    [ -n "$mnts" ] && reasons="$reasons 有挂载/swap($mnts)"
    if grep -qE "^/dev/${d}(p[0-9]+)?[[:space:]]" /proc/swaps 2>/dev/null; then
        reasons="$reasons 是swap"
    fi
    if command -v pvs >/dev/null 2>&1; then
        local pv_hit
        pv_hit=$(pvs --noheadings -o pv_name 2>/dev/null \
                 | grep -E "/dev/${d}(p[0-9]+)?[[:space:]]*$")
        [ -n "$pv_hit" ] && reasons="$reasons 是LVM-PV($pv_hit)"
    fi
    if grep -qE "[[:space:]]${d}(p[0-9]+)?\[[0-9]+\]" /proc/mdstat 2>/dev/null; then
        reasons="$reasons 是md阵列成员"
    fi
    echo "$reasons"
}

# 收集所有“系统盘”的 BDF（挂载来源 / LVM PV / md 成员）
protected_bdfs() {
    local src base disk bdf
    findmnt -rn -o SOURCE 2>/dev/null | grep '^/dev/nvme' | while read -r src; do
        base=${src#/dev/}
        disk=${base%%p[0-9]*}
        bdf=$(get_bdf "$disk")
        [ -n "$bdf" ] && echo "$bdf"
    done
    if command -v pvs >/dev/null 2>&1; then
        pvs --noheadings -o pv_name 2>/dev/null | while read -r pv; do
            [ -n "$pv" ] || continue
            base=${pv#/dev/}
            disk=${base%%p[0-9]*}
            bdf=$(get_bdf "$disk")
            [ -n "$bdf" ] && echo "$bdf"
        done
    fi
    grep -oE '[[:space:]](nvme[0-9]+n[0-9]+)(p[0-9]+)?\[' /proc/mdstat 2>/dev/null \
        | tr -d ' [' | while read -r disk; do
            bdf=$(get_bdf "$disk")
            [ -n "$bdf" ] && echo "$bdf"
        done
    return 0
}

driver_of() {
    readlink -f "/sys/bus/pci/devices/$1/driver" 2>/dev/null || true
}

# 根分区必须仍是 rw（bind/unbind 错盘的实测事故，附录 F-1）
check_root_rw() {
    local opts
    opts=$(findmnt -no OPTIONS / 2>/dev/null || true)
    case ",${opts}," in
        *,rw,*)
            ok "根分区仍为 rw"
            ;;
        *)
            echo "[ABORT] 根分区变只读！很可能误碰了系统盘。" >&2
            echo "恢复：把误接管的盘从 vfio-pci unbind 回 nvme 驱动，然后重启（journal replay 才能回 rw），见部署文档附录 F-1" >&2
            exit 1
            ;;
    esac
}

# 还原残留：把被 vfio-pci 占用、或游离（无驱动）的 NVMe 控制器全部绑回 nvme 驱动。
# 上一次运行失败/中断会把盘留在 vfio-pci 或无驱动状态，接管前必须先还原，
# 保证本次是全新的 unbind + bind。
restore_vfio_nvme() {
    local devpath bdf cls drv vid_did st
    local -a restore_list=()
    for devpath in /sys/bus/pci/devices/*; do
        bdf=$(basename "$devpath")
        cls=$(cat "$devpath/class" 2>/dev/null || true)
        case "$cls" in 0x0108*) ;; *) continue ;; esac
        drv=$(driver_of "$bdf")
        if [ "$drv" = "/sys/bus/pci/drivers/vfio-pci" ] || [ -z "$drv" ]; then
            restore_list+=("$bdf")
        fi
    done
    if [ "${#restore_list[@]}" -eq 0 ]; then
        info "没有 vfio-pci 残留/游离的 NVMe 盘，跳过还原"
        return 0
    fi
    echo "发现上次运行残留的 NVMe 盘，将还原回 nvme 驱动："
    for bdf in "${restore_list[@]}"; do
        drv=$(driver_of "$bdf")
        if [ "$drv" = "/sys/bus/pci/drivers/vfio-pci" ]; then st="vfio-pci 占用"; else st="游离(无驱动)"; fi
        echo "  $bdf  $(lspci -nns "$bdf" 2>/dev/null | cut -d' ' -f2-)  [$st]"
    done
    confirm "还原以上盘?" || abort "用户取消"
    lsmod | grep -q '^nvme' || modprobe nvme 2>/dev/null || true
    for bdf in "${restore_list[@]}"; do
        # 先清掉残留的 driver_override（上次运行可能死在 unbind 和 bind 之间）：
        # override 指着 vfio-pci 时，PCI 核心会拒绝绑回 nvme
        echo "" > "/sys/bus/pci/devices/$bdf/driver_override" 2>/dev/null || true
    done
    for bdf in "${restore_list[@]}"; do
        echo "$bdf" > /sys/bus/pci/drivers/vfio-pci/unbind 2>/dev/null
        if echo "$bdf" > /sys/bus/pci/drivers/nvme/bind 2>/dev/null; then
            ok "$bdf 已还原到 nvme 驱动"
        else
            warn "$bdf 绑回 nvme 失败，请人工检查（modprobe nvme 后重试）"
        fi
    done
}

# -r 还原模式：只做残留清理（上次运行失败后的补救），不做接管
# （必须放在函数定义之后，bash 函数先定义后调用）
if [ "$RESTORE_ONLY" = 1 ]; then
    echo "=== 还原模式：把 vfio-pci 占用/游离的 NVMe 盘还原回 nvme ==="
    _pids="$(pgrep -x nvmf_tgt) $(pgrep -x spdk_tgt) $(pgrep -f 'bin/nvmf_tgt')"
    if [ -n "${_pids// /}" ]; then
        abort "已有 SPDK 应用在运行 (PID: $_pids)，先 kill 再还原（否则残留盘被占用，unbind 会失败）"
    fi
    lsmod | grep -q '^vfio_pci' || modprobe vfio-pci 2>/dev/null || true
    restore_vfio_nvme
    check_root_rw
    info "还原完成"
    exit 0
fi

#===============================================================================
echo "=== 1) 系统盘分布（这些盘/BDF 绝对不能碰）==="
lsblk
echo
echo "--- 挂载来源 ---"
findmnt -rn -o SOURCE,TARGET,FSTYPE 2>/dev/null | grep -E '^/dev/' || true
echo
echo "--- LVM PV → 盘 → BDF ---"
if command -v pvs >/dev/null 2>&1; then
    pvs --noheadings -o pv_name,vg_name 2>/dev/null | while read -r pv vg; do
        [ -n "$pv" ] || continue
        base=${pv#/dev/}
        disk=${base%%p[0-9]*}
        bdf=$(get_bdf "$disk")
        echo "  $pv (VG:${vg:-?}) → $disk → BDF ${bdf:-?}"
    done
fi
echo
echo "--- md 阵列 ---"
grep -vE '^(Personalities|unused)' /proc/mdstat 2>/dev/null || true
echo

echo "=== 2) NVMe 盘候选分析 ==="
printf '%-10s %-14s %-6s %s\n' "盘" "BDF" "状态" "说明"
for dev in /sys/block/nvme*n; do
    d=$(basename "$dev")
    bdf=$(get_bdf "$d")
    reasons=$(disk_reasons "$d")
    if [ -n "$reasons" ]; then st="禁碰"; else st="空闲"; fi
    printf '%-10s %-14s %-6s %s\n' "$d" "${bdf:-?}" "$st" "${reasons:--}"
done
echo

#---------------- 只分析模式 ----------------
if [ -z "$DISKS" ]; then
    info "未指定 -d，到此为止（未做任何变更）。选定“空闲”盘后："
    info "  $0 -d <盘名>[,<盘名>...] [-R strip_kb] [-I iobuf四元组] [-O max_io_size] [-s 计时秒数] [-i listener IP]"
    exit 0
fi

#---------------- 接管模式 ----------------
# 拆盘名列表：逗号分隔 + 多次 -d 都已归并到 $DISKS
IFS=',' read -ra DISK_LIST <<< "$DISKS"
[ "${#DISK_LIST[@]}" -ge 1 ] || abort "-d 解析失败"

# 先全部校验、再动手：任何一块盘不安全就在碰 sysfs 之前 abort
declare -a BDFS=() CTRLRS=()
declare -A SEEN_BDF=()
for _idx in "${!DISK_LIST[@]}"; do
    DISK="${DISK_LIST[$_idx]}"
    case "$DISK" in
        nvme[0-9]*n[0-9]*) ;;
        *) abort "-d 需要整盘名（如 nvme3n1），不能是分区：$DISK" ;;
    esac
    [ -e "/sys/block/$DISK" ] || abort "找不到 /sys/block/$DISK（盘名开机重排，用 lsblk 现查后重跑）"

    BDF=$(get_bdf "$DISK")
    [ -n "$BDF" ] || abort "无法从 sysfs 解析 $DISK 的 BDF"

    REASONS=$(disk_reasons "$DISK")
    [ -n "$REASONS" ] && abort "$DISK 不是空闲盘：$REASONS"

    if protected_bdfs | grep -qx "$BDF"; then
        abort "$BDF ($DISK) 属于系统盘（挂载/LVM-PV/md 成员），绝对不能碰"
    fi

    # 同一控制器的两个 namespace（nvme3n1/nvme3n2）BDF 相同：绑一次就是整盘，重复传会双 attach
    if [ -n "${SEEN_BDF[$BDF]:-}" ]; then
        abort "$DISK 与 ${SEEN_BDF[$BDF]} 是同一控制器（BDF $BDF）：接管按整盘进行，不要重复传"
    fi
    SEEN_BDF[$BDF]="$DISK"

    LSPCI_LINE=$(lspci -nns "$BDF") || abort "lspci 找不到 $BDF"
    BDFS+=("$BDF")
    CTRLRS+=("Nvme$_idx")
    info "盘[$_idx] $DISK → BDF $BDF  $LSPCI_LINE  model=$(lsblk -dno MODEL "/dev/$DISK" 2>/dev/null)"
done

echo
echo "--------------------------------------------------------------"
echo "即将把以下 ${#DISK_LIST[@]} 块盘从 nvme 驱动接管到 vfio-pci (noiommu)："
for _idx in "${!DISK_LIST[@]}"; do
    echo "  ${DISK_LIST[$_idx]} (BDF ${BDFS[$_idx]}) → ${CTRLRS[$_idx]}"
done
echo "之后这些盘无法再作为块设备访问；请再次确认都不是系统盘。"
[ "$RAID0_STRIP" -ge 0 ] && echo "命名空间模式：raid0（$RAID_NAME，strip=$RAID0_STRIP KB）单 namespace"
[ "$RAID0_STRIP" -lt 0 ] && echo "命名空间模式：每盘一个 namespace（nsid 1..${#DISK_LIST[@]}）"
[ -n "$IOBUF_SPEC" ] && echo "iobuf 池：$IOBUF_SPEC（--wait-for-rpc 启动 + iobuf_set_options）"
[ "$MAX_IO_SIZE" -gt 0 ] && echo "transport max_io_size：$MAX_IO_SIZE 字节"
confirm "继续?" || abort "用户取消"
echo

echo "=== 3) vfio-pci noiommu 接管 ==="
lsmod | grep -q '^vfio_pci' || modprobe vfio-pci || abort "modprobe vfio-pci 失败"
lsmod | grep -q '^vfio'     || modprobe vfio     || abort "modprobe vfio 失败"

# enable_unsafe_noiommu_mode 在主线内核里属于 vfio 核心模块
# （/sys/module/vfio/parameters/），vfio_iommu_type1 下只有
# allow_unsafe_interrupts 等；个别厂商内核位置不同，两处都找一遍（附录 F-2）
NOIOMMU=""
for _c in /sys/module/vfio/parameters/enable_unsafe_noiommu_mode \
          /sys/module/vfio_iommu_type1/parameters/enable_unsafe_noiommu_mode; do
    [ -e "$_c" ] && NOIOMMU=$_c && break
done
[ -n "$NOIOMMU" ] \
    || abort "两个模块目录下都找不到 enable_unsafe_noiommu_mode（试: find /sys/module -name 'enable_unsafe_noiommu_mode'；需 CONFIG_VFIO_NOIOMMU=y 且 vfio 已加载）"
if [ "$(cat "$NOIOMMU")" != "1" ]; then
    echo 1 > "$NOIOMMU" 2>/dev/null || abort "写 $NOIOMMU 失败"
    ok "noiommu 模式已开启（$NOIOMMU=1）"
else
    info "noiommu 模式已是 1（$NOIOMMU）"
fi

# 上次运行失败会把盘留在 vfio-pci/游离状态：nvmf_tgt 必须先停（可能占着残留盘），
# 再把残留盘还原回 nvme，确保本次是全新的 unbind + bind。
# 注意旧实例不一定叫 nvmf_tgt（改名/别的 SPDK 应用也会占 core mask），多查几种
_pids="$(pgrep -x nvmf_tgt) $(pgrep -x spdk_tgt) $(pgrep -f 'bin/nvmf_tgt')"
if [ -n "${_pids// /}" ]; then
    abort "已有 SPDK 应用在运行 (PID: $_pids)，先 kill 再来（占着 core mask，且 RPC 配置不持久化）"
fi
restore_vfio_nvme
check_root_rw

# 逐盘 driver_override 接管（SPDK setup.sh 的标准做法，不依赖 new_id 动态 ID 表——
# 部分 openEuler 内核对 new_id 写 vendor:device 直接回 EINVAL，node1 实测）。
# override 先写后 unbind：万一解绑瞬间被 probe，也只有 override 指定的 vfio-pci 能匹配，
# 不会漂回 nvme。每绑完一块立刻 check_root_rw（错盘事故要当场暴露）
for _idx in "${!DISK_LIST[@]}"; do
    DISK="${DISK_LIST[$_idx]}"
    BDF="${BDFS[$_idx]}"
    DRV=$(driver_of "$BDF")
    if [ "$DRV" = "/sys/bus/pci/drivers/vfio-pci" ]; then
        info "$BDF ($DISK) 已被 vfio-pci 接管，跳过"
        continue
    fi
    [ "$DRV" = "/sys/bus/pci/drivers/nvme" ] \
        || abort "$BDF ($DISK) 当前驱动是 ${DRV:-无}，不是 nvme，请人工确认后再操作"

    echo "vfio-pci" > "/sys/bus/pci/devices/$BDF/driver_override" 2>/dev/null \
        || abort "写 $BDF driver_override 失败"
    echo "$BDF" > /sys/bus/pci/drivers/nvme/unbind 2>/dev/null \
        || abort "unbind $BDF 失败"
    if ! echo "$BDF" > /sys/bus/pci/drivers/vfio-pci/bind 2>/tmp/.bind.err; then
        echo "" > "/sys/bus/pci/devices/$BDF/driver_override" 2>/dev/null || true
        DRV=$(driver_of "$BDF")
        [ "$DRV" = "/sys/bus/pci/drivers/vfio-pci" ] \
            || abort "bind 失败: $(cat /tmp/.bind.err 2>/dev/null)（盘当前驱动: ${DRV:-无}，可重跑本脚本或 -r 还原）"
    fi
    # 已归 vfio-pci，清掉 override，免得 restore 时挡住绑回 nvme
    echo "" > "/sys/bus/pci/devices/$BDF/driver_override" 2>/dev/null || true

    DRV=$(driver_of "$BDF")
    [ "$DRV" = "/sys/bus/pci/drivers/vfio-pci" ] || abort "$DISK 接管失败，当前驱动: ${DRV:-无}"
    ok "vfio-pci 已接管 $DISK：$BDF"
    check_root_rw
done

[ -e /dev/vfio/vfio ] || abort "/dev/vfio/vfio 不存在"
GRP_CNT=$(find /dev/vfio -maxdepth 1 -type c ! -name vfio 2>/dev/null | wc -l)
[ "$GRP_CNT" -ge 1 ] || warn "/dev/vfio/ 下没有组设备，SPDK 可能打不开这些盘"
echo

echo "=== 4) 环境与前置检查 ==="
if [ -n "$DUMP_SEC" ]; then
    case "$DUMP_SEC" in
        ''|*[!0-9]*) abort "-s 需要正整数秒" ;;
    esac
    [ "$DUMP_SEC" -ge 1 ] || abort "-s 需要正整数秒"
fi

# -I 四元组校验：小池数量,小池buf,大池数量,大池buf（对照 lib/thread/iobuf.c 的下限）
IB_SMALL_COUNT=0; IB_SMALL_SIZE=0; IB_LARGE_COUNT=0; IB_LARGE_SIZE=0
if [ -n "$IOBUF_SPEC" ]; then
    IFS=',' read -ra _IB <<< "$IOBUF_SPEC"
    [ "${#_IB[@]}" -eq 4 ] || abort "-I 需要 4 个逗号分隔数字：small_count,small_size,large_count,large_size"
    for _v in "${_IB[@]}"; do
        case "$_v" in ''|*[!0-9]*) abort "-I 的每个字段都必须是非负整数，收到: $_v" ;; esac
    done
    IB_SMALL_COUNT=${_IB[0]}; IB_SMALL_SIZE=${_IB[1]}; IB_LARGE_COUNT=${_IB[2]}; IB_LARGE_SIZE=${_IB[3]}
    [ "$IB_SMALL_COUNT" -ge 64 ]  || abort "small_pool_count 最小 64（IOBUF_MIN_SMALL_POOL_SIZE）"
    [ "$IB_LARGE_COUNT" -ge 8 ]   || abort "large_pool_count 最小 8（IOBUF_MIN_LARGE_POOL_SIZE）"
    [ "$IB_SMALL_SIZE"  -ge 4096 ] || abort "small_bufsize 最小 4096（IOBUF_MIN_SMALL_BUFSIZE）"
    [ "$IB_LARGE_SIZE"  -ge 8192 ] || abort "large_bufsize 最小 8192（IOBUF_MIN_LARGE_BUFSIZE）"

    # 池内存 = 小池 + 大池 + 1GiB 余量，换算成 2MB hugepages；不够就自动上调
    _pool_bytes=$(( IB_SMALL_COUNT * IB_SMALL_SIZE + IB_LARGE_COUNT * IB_LARGE_SIZE ))
    _need_pages=$(( (_pool_bytes + 1073741824 + 2097151) / 2097152 ))
    [ "$_need_pages" -gt "$HUGE_PAGES" ] && HUGE_PAGES=$_need_pages
    info "iobuf 池内存 ≈ $((_pool_bytes / 1048576)) MiB（hugepages 目标上调为 $HUGE_PAGES 页 ≈ $((HUGE_PAGES / 512)) GiB）"
fi

# -O 校验：2 的幂且 ≥8KB（与 transport 层一致）；且必须 ≤ large_bufsize
# —— urma 数据路径要求 iovcnt==1（urma.c nvmf_urma_post_data），I/O 大于 large_bufsize
#    会被拆成多个 iobuf buffer，直接 -ENOTSUP 失败，所以这里提前把配置卡死
if [ "$MAX_IO_SIZE" -gt 0 ]; then
    [ $((MAX_IO_SIZE & (MAX_IO_SIZE - 1))) -eq 0 ] && [ "$MAX_IO_SIZE" -ge 8192 ] \
        || abort "-O max_io_size 必须是 2 的幂且 ≥8KB"
    if [ -n "$IOBUF_SPEC" ] && [ "$IB_LARGE_SIZE" -lt "$MAX_IO_SIZE" ]; then
        abort "-O $MAX_IO_SIZE > -I 的 large_bufsize $IB_LARGE_SIZE：urma 要求数据单 buffer（iovcnt==1），请把 -I 第 4 个字段配到 ≥ $MAX_IO_SIZE"
    fi
    if [ -z "$IOBUF_SPEC" ] && [ "$MAX_IO_SIZE" -gt 135168 ]; then
        abort "-O $MAX_IO_SIZE 但未配 -I：默认 large_bufsize 只有 135168，>135168 的 I/O 会因 iovcnt!=1 失败。请加 -I，例如 -I 8192,8192,1024,$MAX_IO_SIZE"
    fi
fi

# SPDK 源码根：-w 优先；否则从脚本所在目录、当前目录分别向上找 build/bin/nvmf_tgt
if [ -z "$SPDK_DIR" ]; then
    for _start in "$(cd "$(dirname "$0")" && pwd)" "$PWD"; do
        _d=$_start
        while [ "$_d" != "/" ]; do
            if [ -x "$_d/build/bin/nvmf_tgt" ]; then
                SPDK_DIR=$_d
                break 2
            fi
            _d=$(dirname "$_d")
        done
    done
fi
[ -x "$SPDK_DIR/build/bin/nvmf_tgt" ] \
    || abort "自动探测不到 SPDK 源码根（脚本目录、当前目录及其上级都没有 build/bin/nvmf_tgt）。用 -w <SPDK源码根> 指定"
[ -x "$SPDK_DIR/scripts/rpc.py" ] \
    || abort "在 $SPDK_DIR 下找不到 scripts/rpc.py"

ls "$LIBDIR" 2>/dev/null | grep -q '^liburma\.so' || abort "$LIBDIR 里没有 liburma.so*"
if ! ls "$LIBDIR" 2>/dev/null | grep -q '^liburma_common\.so'; then
    warn "$LIBDIR 缺 liburma_common.so*！liburma.so.0 依赖它，缺了会回落 /usr/lib64 旧版"
    warn "→ urma_init() 返回 4096 → nvmf_create_transport 静默失败（部署文档附录 F-3）"
    confirm "仍要继续?" || abort "换用成套库目录重跑：-L <同时含 liburma* 与 liburma_common* 的目录>"
fi

if [ "$MAX_IO_SIZE" -gt 0 ] && [ "$IB_LARGE_COUNT" -gt 0 ] && [ "$IB_LARGE_COUNT" -lt 256 ]; then
    # transport 缓存已显式压小（每 PG 32 large），pool=160 时预占 64+32=96、剩 64，
    # 单核 32 个在飞 4MB I/O 够用；T/b 更大或 -R raid0 时余量变薄，建议 256（1GiB）
    warn "-O 大 I/O 时 large_pool_count=$IB_LARGE_COUNT 偏小，建议 ≥256（-I 第 3 个字段）"
fi

TOTAL_HP=$(awk '/^HugePages_Total/{print $2}' /proc/meminfo)
if [ "${TOTAL_HP:-0}" -lt "$HUGE_PAGES" ]; then
    info "HugePages_Total=${TOTAL_HP:-0} < $HUGE_PAGES，尝试补齐"
    echo "$HUGE_PAGES" > /proc/sys/vm/nr_hugepages 2>/dev/null || warn "写 nr_hugepages 失败"
    TOTAL_HP=$(awk '/^HugePages_Total/{print $2}' /proc/meminfo)
    [ "${TOTAL_HP:-0}" -ge "$HUGE_PAGES" ] || abort "HugePages 不足($TOTAL_HP)，nvmf_tgt 起不来"
else
    info "HugePages_Total=$TOTAL_HP 足够"
fi
echo

# listener IP：-i 优先，否则自动探测第一个全局 IPv4（多网卡机器务必用 -i 显式指定）
if [ -z "$LISTEN_IP" ]; then
    LISTEN_IP=$(ip -4 -o addr show scope global 2>/dev/null \
                | awk '{sub(/\/.*/,"",$4); print $4; exit}')
fi
[ -n "$LISTEN_IP" ] || abort "无法自动探测本机 IP，用 -i <IP> 指定 listener 地址"

echo "=== 5) 后台启动 nvmf_tgt ==="
cd "$SPDK_DIR" || abort "cd $SPDK_DIR 失败"
export LD_LIBRARY_PATH="$LIBDIR:${LD_LIBRARY_PATH:-}"
export SPDK_URMA_DEV_NAME="$DEVNAME"
info "LD_LIBRARY_PATH=$LIBDIR  SPDK_URMA_DEV_NAME=$DEVNAME  core mask=$COREMASK$( [ -n "$DUMP_SEC" ] && echo "  SPDK_URMA_TARGET_DUMP_SEC=$DUMP_SEC")$( [ -n "$IOBUF_SPEC" ] && echo "  --wait-for-rpc")"

# -I 需要在子系统初始化前设 iobuf 池（iobuf_set_options 仅 SPDK_RPC_STARTUP 可调），
# 所以先以 --wait-for-rpc 启动，RPC 配完池子再 framework_start_init
TGT_ARGS=(-m "$COREMASK")
[ -n "$IOBUF_SPEC" ] && TGT_ARGS+=(--wait-for-rpc)
nohup env $( [ -n "$DUMP_SEC" ] && echo "SPDK_URMA_TARGET_DUMP_SEC=$DUMP_SEC" ) \
    ./build/bin/nvmf_tgt "${TGT_ARGS[@]}" > "$TGT_LOG" 2>&1 &
TGT_PID=$!
echo "$TGT_PID" > "$TGT_PIDFILE"
info "nvmf_tgt 已后台启动 PID=$TGT_PID（日志: $TGT_LOG；停止: kill \$(cat $TGT_PIDFILE)）"

RPC="$SPDK_DIR/scripts/rpc.py"
info "等待 RPC 就绪..."
READY=0
for _ in $(seq 1 60); do
    # 必须先确认"我们自己启动的"进程还活着，再测 RPC——否则新进程秒退时，
    # 机器上旧实例的 /var/tmp/spdk.sock 会替它应答，RPC 配置会打到旧实例上
    if ! kill -0 "$TGT_PID" 2>/dev/null; then
        echo "---- nvmf_tgt 日志尾部 ----" >&2
        tail -n 30 "$TGT_LOG" >&2
        # SPDK 抢不到 core mask 时日志里有 "probably process <pid> has claimed it"
        _holder=$(grep -oE 'probably process [0-9]+' "$TGT_LOG" 2>/dev/null | grep -oE '[0-9]+' | tail -n1)
        if [ -n "$_holder" ]; then
            abort "nvmf_tgt 退出：core mask 被旧 SPDK 实例 PID $_holder 占用。先看是谁（ps -p $_holder -o pid,comm,args,etime），确认后 kill $_holder 再重跑"
        fi
        abort "nvmf_tgt 进程退出，启动失败"
    fi
    if "$RPC" rpc_get_methods >/dev/null 2>&1; then READY=1; break; fi
    sleep 0.5
done
[ "$READY" = 1 ] || { tail -n 30 "$TGT_LOG" >&2; abort "30s 内 RPC 未就绪"; }
ok "RPC 就绪（PID $TGT_PID）"

run_rpc() {
    info "\$ rpc.py $*"
    if ! "$RPC" "$@"; then
        echo "---- nvmf_tgt 日志尾部 ----" >&2
        tail -n 15 "$TGT_LOG" >&2
        abort "RPC 失败: $*"
    fi
}

if [ -n "$IOBUF_SPEC" ]; then
    echo "=== 6) 配置 iobuf 池（startup 窗口） ==="
    run_rpc iobuf_set_options \
        --small-pool-count "$IB_SMALL_COUNT" --small-bufsize "$IB_SMALL_SIZE" \
        --large-pool-count "$IB_LARGE_COUNT" --large-bufsize "$IB_LARGE_SIZE"
    run_rpc framework_start_init
    ok "iobuf 池已生效（small ${IB_SMALL_COUNT}x${IB_SMALL_SIZE}B，large ${IB_LARGE_COUNT}x${IB_LARGE_SIZE}B）"
fi

echo "=== 7) 配置 RPC ==="
# 每块盘一个 controller（Nvme0/Nvme1/...）
for _idx in "${!DISK_LIST[@]}"; do
    run_rpc bdev_nvme_attach_controller -b "${CTRLRS[$_idx]}" -t PCIe -a "${BDFS[$_idx]}"
done
if [ "$MAX_IO_SIZE" -gt 0 ]; then
    # transport 的 iobuf 缓存默认"自动吃大池的一半再按已有 PG 数均分"（transport.c:640），
    # 第一个 PG 独吞 pool/2，叠加每核 bdev(16)+accel(16) 个 large 预占后，
    # add_ns 建通道时池子必被抽干（0/16 失败）。显式压小：每 PG 固定 32 个 large
    # （够单核 32 个在飞 I/O），small 1024 同理封顶，余量留给共享池。
    run_rpc nvmf_create_transport -t URMA -i "$MAX_IO_SIZE" \
        --iobuf-large-cache-size 32 --iobuf-small-cache-size 1024
else
    run_rpc nvmf_create_transport -t URMA
fi
run_rpc nvmf_create_subsystem "$NQN" -a -s "$SUBSYS_SN"

if [ "$RAID0_STRIP" -ge 0 ]; then
    # raid0：把所有 base bdev 拼成一个大 bdev，单 namespace 聚合带宽
    _bases=""
    for _c in "${CTRLRS[@]}"; do _bases+="${_c}n1 "; done
    _raid_args=(-n "$RAID_NAME" -r raid0 -b "$_bases")
    [ "$RAID0_STRIP" -gt 0 ] && _raid_args+=(-z "$RAID0_STRIP")
    run_rpc bdev_raid_create "${_raid_args[@]}"
    run_rpc nvmf_subsystem_add_ns "$NQN" "$RAID_NAME" -n 1
    info "namespace: $RAID_NAME（raid0，成员: $_bases）→ nsid 1"
else
    # 多 namespace：每盘一个 bdev，nsid 依次 1..N（同一 subsystem 内 nsid 必须唯一）
    for _idx in "${!DISK_LIST[@]}"; do
        run_rpc nvmf_subsystem_add_ns "$NQN" "${CTRLRS[$_idx]}n1" -n $((_idx + 1))
    done
fi
run_rpc nvmf_subsystem_add_listener "$NQN" -t URMA -f IPv4 -a "$LISTEN_IP" -s "$LISTEN_PORT"
ok "全部 RPC 配置完成"

echo
echo "================================================================"
echo "Target 就绪："
for _idx in "${!DISK_LIST[@]}"; do
    echo "  NVMe[$_idx] : ${DISK_LIST[$_idx]} (BDF ${BDFS[$_idx]}) → ${CTRLRS[$_idx]}"
done
if [ "$RAID0_STRIP" -ge 0 ]; then
    echo "  namespace  : $RAID_NAME（raid0）→ nsid 1"
else
    echo "  namespaces : ${CTRLRS[*]}n1 → nsid 1..${#DISK_LIST[@]}"
fi
echo "  subsystem  : $NQN"
echo "  listener   : $LISTEN_IP:$LISTEN_PORT (URMA)"
[ -n "$IOBUF_SPEC" ] && echo "  iobuf      : small ${IB_SMALL_COUNT}x${IB_SMALL_SIZE}B / large ${IB_LARGE_COUNT}x${IB_LARGE_SIZE}B"
[ "$MAX_IO_SIZE" -gt 0 ] && echo "  max_io_size: $MAX_IO_SIZE B（transport）"
echo "  nvmf_tgt   : PID $TGT_PID，日志 $TGT_LOG$( [ -n "$DUMP_SEC" ] && echo "；target 打点每 ${DUMP_SEC}s 输出一次（==== URMA target timing breakdown ====）")"
echo "  停止       : kill \$(cat $TGT_PIDFILE)"
echo
echo "151（Initiator）上测试："
if [ "$MAX_IO_SIZE" -gt 0 ]; then
    echo "  # transport max_io_size=$MAX_IO_SIZE，initiator 侧必须一致（否则 hello 协商后按小的算）："
    echo "  export SPDK_URMA_MAX_IO_SIZE=$MAX_IO_SIZE"
fi
echo "  sudo LD_LIBRARY_PATH=<成套库目录> SPDK_URMA_DEV_NAME=$DEVNAME \\"
echo "      ./build/examples/urma_perf \\"
echo "      -r 'trtype:URMA adrfam:IPv4 traddr:$LISTEN_IP trsvcid:$LISTEN_PORT subnqn:$NQN' \\"
echo "      -w write -o 4096 -T 4 -b 32 -t 30 -n 1 -g 0 -l 0 -M posix"
if [ "$RAID0_STRIP" -lt 0 ] && [ "${#DISK_LIST[@]}" -gt 1 ]; then
    echo "  # 多 namespace 模式：所有 ns 在一个 ctrlr 下，perf 线程会自动摊到各 ns；"
    echo "  # 想单独压某个盘用 urma_perf 的 namespace 选择（或临时去掉其他 ns）"
fi
echo "================================================================"
