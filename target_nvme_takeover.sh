#!/bin/bash
#===============================================================================
# target_nvme_takeover.sh — Target 节点 NVMe 接管（vfio-pci noiommu）+ nvmf_tgt + RPC 一键脚本
#
# 流程：
#   1. lsblk 展示系统盘分布，标出“绝对不能碰”的盘（root/home 挂载、LVM PV、md 成员）
#   2. 分析候选空闲盘；选定一盘后【现查】其 BDF（nvmeXn1 每次开机重排，旧记录不可信）
#   3. lspci -nns 显示 vendor:device，用该 BDF 执行 new_id → unbind → bind
#   4. 验证 vfio-pci 已接管 + 根分区仍是 rw（bind 错盘 = 根文件系统变 ro，实测过）
#   5. 环境检查：liburma 与 liburma_common 成套、hugepages、无残留 nvmf_tgt
#   6. 后台启动 ./build/bin/nvmf_tgt -m <mask>（计时为可选开关 -s 秒数）
#   7. RPC 就绪后自动配置：attach controller（用刚接管的 BDF）→ create transport
#      → subsystem → ns → listener（listener IP 自动探测，多网卡用 -i 显式指定）
#
# 用法：
#   ./target_nvme_takeover.sh                   # 只分析：列系统盘 + 空闲候选盘，不做任何变更
#   ./target_nvme_takeover.sh -d nvme3n1        # 接管 nvme3n1 → 后台启动 nvmf_tgt → 自动配 RPC
#   ./target_nvme_takeover.sh -d nvme3n1 -s 10  # 同上 + SPDK_URMA_TARGET_DUMP_SEC=10 计时
#
# 选项：
#   -d <盘名>    目标 NVMe 盘（如 nvme3n1）；不带则只做分析不接管
#   -s <秒>      打开 target 计时（SPDK_URMA_TARGET_DUMP_SEC，transport 创建时读取）
#   -i <IP>      listener 地址（默认自动探测本机第一个全局 IPv4；多网卡机器建议显式指定）
#   -m <掩码>    nvmf_tgt core mask（默认 0x3）
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

HUGE_PAGES=2048

DISK=""
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
BDEV_CTRL="Nvme0"
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

while getopts "d:s:i:m:w:L:N:ryh" opt; do
    case $opt in
        d) DISK=$OPTARG ;;
        s) DUMP_SEC=$OPTARG ;;
        i) LISTEN_IP=$OPTARG ;;
        m) COREMASK=$OPTARG ;;
        w) SPDK_DIR=$OPTARG ;;
        L) LIBDIR=$OPTARG ;;
        N) DEVNAME=$OPTARG ;;
        r) RESTORE_ONLY=1 ;;
        y) ASSUME_YES=1 ;;
        h) usage ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))

[ "$(id -u)" -eq 0 ] || abort "必须以 root 运行（sysfs 绑定 + hugepages + 启动 nvmf_tgt）"

# -r 还原模式：只做残留清理（上次运行失败后的补救），不做接管
if [ "$RESTORE_ONLY" = 1 ]; then
    echo "=== 还原模式：把 vfio-pci 占用/游离的 NVMe 盘还原回 nvme ==="
    lsmod | grep -q '^vfio_pci' || modprobe vfio-pci 2>/dev/null || true
    restore_vfio_nvme
    check_root_rw
    info "还原完成"
    exit 0
fi

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
    # 先从 vfio-pci 的 ID 表摘掉，防止解绑后被自动 probe 抢回去
    if [ -e /sys/bus/pci/drivers/vfio-pci/remove_id ]; then
        for bdf in "${restore_list[@]}"; do
            vid_did=$(lspci -nns "$bdf" 2>/dev/null \
                      | grep -oE '\[[0-9a-fA-F]{4}:[0-9a-fA-F]{4}\]' | head -n1 | tr -d '[]')
            [ -n "$vid_did" ] && echo "$vid_did" \
                > /sys/bus/pci/drivers/vfio-pci/remove_id 2>/dev/null
        done
    fi
    for bdf in "${restore_list[@]}"; do
        echo "$bdf" > /sys/bus/pci/drivers/vfio-pci/unbind 2>/dev/null
        if echo "$bdf" > /sys/bus/pci/drivers/nvme/bind 2>/dev/null; then
            ok "$bdf 已还原到 nvme 驱动"
        else
            warn "$bdf 绑回 nvme 失败，请人工检查（modprobe nvme 后重试）"
        fi
    done
}

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
if [ -z "$DISK" ]; then
    info "未指定 -d，到此为止（未做任何变更）。选定“空闲”盘后："
    info "  $0 -d <盘名> [-s 计时秒数] [-i listener IP]"
    exit 0
fi

#---------------- 接管模式 ----------------
case "$DISK" in
    nvme[0-9]*n[0-9]*) ;;
    *) abort "-d 需要整盘名（如 nvme3n1），不能是分区" ;;
esac
[ -e "/sys/block/$DISK" ] || abort "找不到 /sys/block/$DISK（盘名开机重排，用 lsblk 现查后重跑）"

BDF=$(get_bdf "$DISK")
[ -n "$BDF" ] || abort "无法从 sysfs 解析 $DISK 的 BDF"

REASONS=$(disk_reasons "$DISK")
[ -n "$REASONS" ] && abort "$DISK 不是空闲盘：$REASONS"

if protected_bdfs | grep -qx "$BDF"; then
    abort "$BDF 属于系统盘（挂载/LVM-PV/md 成员），绝对不能碰"
fi

echo "=== 3) $DISK → BDF $BDF ==="
LSPCI_LINE=$(lspci -nns "$BDF") || abort "lspci 找不到 $BDF"
echo "  $LSPCI_LINE"
VID_DID=$(echo "$LSPCI_LINE" | grep -oE '\[[0-9a-fA-F]{4}:[0-9a-fA-F]{4}\]' \
          | head -n1 | tr -d '[]')
[ -n "$VID_DID" ] || abort "无法从 lspci 解析 vendor:device"
VENDOR=${VID_DID%%:*}
DEVID=${VID_DID##*:}
info "vendor:device = $VID_DID，model = $(lsblk -dno MODEL "/dev/$DISK" 2>/dev/null)"
echo

echo "--------------------------------------------------------------"
echo "即将把 $DISK (BDF=$BDF, $VID_DID) 从 nvme 驱动接管到 vfio-pci (noiommu)"
echo "之后该盘无法再作为块设备访问；请再次确认不是系统盘。"
confirm "继续?" || abort "用户取消"
echo

echo "=== 4) vfio-pci noiommu 接管 ==="
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
# 再把残留盘还原回 nvme，确保本次是全新的 unbind + bind
if pgrep -x nvmf_tgt >/dev/null 2>&1; then
    abort "nvmf_tgt 已在运行 (PID $(pgrep -x nvmf_tgt | tr '\n' ' '))，先 kill 再来（RPC 配置不持久化，需重做）"
fi
restore_vfio_nvme
check_root_rw

if ! echo "$VENDOR $DEVID" > /sys/bus/pci/drivers/vfio-pci/new_id 2>/tmp/.newid.err; then
    if grep -q "File exists" /tmp/.newid.err 2>/dev/null; then
        info "new_id $VID_DID 已注册过，跳过"
    else
        abort "new_id 失败: $(cat /tmp/.newid.err 2>/dev/null)"
    fi
fi

DRV=$(driver_of "$BDF")
if [ "$DRV" = "/sys/bus/pci/drivers/vfio-pci" ]; then
    info "$BDF 已被 vfio-pci 接管，跳过 unbind/bind"
else
    [ "$DRV" = "/sys/bus/pci/drivers/nvme" ] \
        || abort "$BDF 当前驱动是 ${DRV:-无}，不是 nvme，请人工确认后再操作"
    echo "$BDF" > /sys/bus/pci/drivers/nvme/unbind 2>/dev/null \
        || abort "unbind $BDF 失败"
    info "已从 nvme 驱动解绑（unbind 后 new_id 可能已自动 probe 到 vfio-pci，先确认）"
    sleep 0.5
    DRV=$(driver_of "$BDF")
    if [ "$DRV" != "/sys/bus/pci/drivers/vfio-pci" ]; then
        if ! echo "$BDF" > /sys/bus/pci/drivers/vfio-pci/bind 2>/tmp/.bind.err; then
            DRV=$(driver_of "$BDF")
            [ "$DRV" = "/sys/bus/pci/drivers/vfio-pci" ] \
                || abort "bind 失败: $(cat /tmp/.bind.err 2>/dev/null)"
        fi
    fi
fi

DRV=$(driver_of "$BDF")
[ "$DRV" = "/sys/bus/pci/drivers/vfio-pci" ] || abort "接管失败，当前驱动: ${DRV:-无}"
ok "vfio-pci 已接管：$(driver_of "$BDF")"

[ -e /dev/vfio/vfio ] || abort "/dev/vfio/vfio 不存在"
GRP_CNT=$(find /dev/vfio -maxdepth 1 -type c ! -name vfio 2>/dev/null | wc -l)
[ "$GRP_CNT" -ge 1 ] || warn "/dev/vfio/ 下没有组设备，SPDK 可能打不开该盘"

# bind 错盘 = 根文件系统变 ro 的实测事故（附录 F-1），必须立刻检查
check_root_rw
echo

echo "=== 5) 环境与前置检查 ==="
if [ -n "$DUMP_SEC" ]; then
    case "$DUMP_SEC" in
        ''|*[!0-9]*) abort "-s 需要正整数秒" ;;
    esac
    [ "$DUMP_SEC" -ge 1 ] || abort "-s 需要正整数秒"
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

ls "$LIBDIR"/liburma.so* >/dev/null 2>&1 || abort "$LIBDIR 里没有 liburma.so*"
if ! ls "$LIBDIR"/liburma_common.so* >/dev/null 2>&1; then
    warn "$LIBDIR 缺 liburma_common.so*！liburma.so.0 依赖它，缺了会回落 /usr/lib64 旧版"
    warn "→ urma_init() 返回 4096 → nvmf_create_transport 静默失败（部署文档附录 F-3）"
    confirm "仍要继续?" || abort "换用成套库目录重跑：-L <同时含 liburma* 与 liburma_common* 的目录>"
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

echo "=== 6) 后台启动 nvmf_tgt ==="
cd "$SPDK_DIR" || abort "cd $SPDK_DIR 失败"
export LD_LIBRARY_PATH="$LIBDIR:${LD_LIBRARY_PATH:-}"
export SPDK_URMA_DEV_NAME="$DEVNAME"
info "LD_LIBRARY_PATH=$LIBDIR  SPDK_URMA_DEV_NAME=$DEVNAME  core mask=$COREMASK$( [ -n "$DUMP_SEC" ] && echo "  SPDK_URMA_TARGET_DUMP_SEC=$DUMP_SEC")"

if [ -n "$DUMP_SEC" ]; then
    nohup env SPDK_URMA_TARGET_DUMP_SEC="$DUMP_SEC" \
        ./build/bin/nvmf_tgt -m "$COREMASK" > "$TGT_LOG" 2>&1 &
else
    nohup ./build/bin/nvmf_tgt -m "$COREMASK" > "$TGT_LOG" 2>&1 &
fi
TGT_PID=$!
echo "$TGT_PID" > "$TGT_PIDFILE"
info "nvmf_tgt 已后台启动 PID=$TGT_PID（日志: $TGT_LOG；停止: kill \$(cat $TGT_PIDFILE)）"

RPC="$SPDK_DIR/scripts/rpc.py"
info "等待 RPC 就绪..."
READY=0
for _ in $(seq 1 60); do
    if "$RPC" rpc_get_methods >/dev/null 2>&1; then READY=1; break; fi
    if ! kill -0 "$TGT_PID" 2>/dev/null; then
        echo "---- nvmf_tgt 日志尾部 ----" >&2
        tail -n 30 "$TGT_LOG" >&2
        abort "nvmf_tgt 进程退出，启动失败"
    fi
    sleep 0.5
done
[ "$READY" = 1 ] || { tail -n 30 "$TGT_LOG" >&2; abort "30s 内 RPC 未就绪"; }
ok "RPC 就绪"

echo "=== 7) 配置 RPC ==="
run_rpc() {
    info "\$ rpc.py $*"
    if ! "$RPC" "$@"; then
        echo "---- nvmf_tgt 日志尾部 ----" >&2
        tail -n 15 "$TGT_LOG" >&2
        abort "RPC 失败: $*"
    fi
}
run_rpc bdev_nvme_attach_controller -b "$BDEV_CTRL" -t PCIe -a "$BDF"
run_rpc nvmf_create_transport -t URMA
run_rpc nvmf_create_subsystem "$NQN" -a -s "$SUBSYS_SN"
run_rpc nvmf_subsystem_add_ns "$NQN" "${BDEV_CTRL}n1" -n 1
run_rpc nvmf_subsystem_add_listener "$NQN" -t URMA -f IPv4 -a "$LISTEN_IP" -s "$LISTEN_PORT"
ok "全部 RPC 配置完成"

echo
echo "================================================================"
echo "Target 就绪："
echo "  NVMe      : $DISK (BDF $BDF) → $BDEV_CTRL"
echo "  subsystem : $NQN"
echo "  listener  : $LISTEN_IP:$LISTEN_PORT (URMA)"
echo "  nvmf_tgt  : PID $TGT_PID，日志 $TGT_LOG$( [ -n "$DUMP_SEC" ] && echo "；target 打点每 ${DUMP_SEC}s 输出一次（==== URMA target timing breakdown ====）")"
echo "  停止      : kill \$(cat $TGT_PIDFILE)"
echo
echo "151（Initiator）上测试："
echo "  sudo LD_LIBRARY_PATH=<成套库目录> SPDK_URMA_DEV_NAME=$DEVNAME \\"
echo "      ./build/examples/urma_perf \\"
echo "      -r 'trtype:URMA adrfam:IPv4 traddr:$LISTEN_IP trsvcid:$LISTEN_PORT subnqn:$NQN' \\"
echo "      -w write -o 4096 -T 4 -b 32 -t 30 -n 1 -g 0 -l 0 -M posix"
echo "================================================================"
