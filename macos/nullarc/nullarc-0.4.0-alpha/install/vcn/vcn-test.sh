#!/bin/bash
# Drive the hard-coded VCN 4.0.2 bring-up after the reboot that installed it.
#   sudo bash vcn-test.sh           # status, init, ring and fences across wrap, power down
#   sudo bash vcn-test.sh --keep    # leave VCN powered for poking at it afterwards
#   sudo bash vcn-test.sh --tmr 0x8008000000   # skip the PSP load, use a preloaded TMR (see the boot fallback)
# A reboot is armed for 4 minutes in case the GPU hangs hard; it is cancelled when the test finishes.
set -u
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
G=/Users/Shared/gpu
C="$G/vcnctl"
FW="$G/fw/vcn_4_0_0.bin"
KEEP=0
TMR_ARG=""
while [ $# -gt 0 ]; do
    case "$1" in
    --keep) KEEP=1 ;;
    --tmr) [ "$#" -ge 2 ] || { echo "--tmr requires a firmware MC address"; exit 2; }
           shift; TMR_ARG="$1" ;;
    *) echo "unknown argument $1"; exit 2 ;;
    esac
    shift
done
[ -f "$FW" ] || { echo "missing $FW: run install-vcn.sh first"; exit 1; }
BUILD=$(ioreg -r -l -c PhxBringup -d 1 | awk -F'= ' '/"phx-build"/{print $2; exit}')
echo "== PhxBringup build: ${BUILD:-<not loaded>}"
case "$BUILD" in
    38|62|63|64|65|66|67|68|75|76|78|79|80|81|82) ;;
    60|61) C="$G/vcnctl" ;;
    *) echo "This test requires PhxBringup build 38 or 60 through 68. Loaded: ${BUILD:-unknown}."; exit 1 ;;
esac
[ -x "$C" ] || { echo "missing vcnctl at $C"; exit 1; }
if [ "$BUILD" -ge 62 ]; then
    [ -x "$G/vcn_frame_check" ] || { echo "missing vcn_frame_check: run install-vcn.sh --tool-only"; exit 1; }
    RUN_DIR=$(mktemp -d "$G/vcn-run.XXXXXXXX") || exit 1
    echo "== hardware test log and frame output: $RUN_DIR"
    exec > >(tee "$RUN_DIR/run.log") 2>&1
    echo "PhxBringup build $BUILD"
fi
echo "== firmware header check"
if ! "$C" check-firmware "$FW"; then
    echo "Firmware header check failed for $FW using $C"
    exit 1
fi
echo "== watchdog: reboot in 4 min unless cancelled"
(
    trap 'kill "$timer" 2>/dev/null; exit' TERM
    sleep 240 &
    timer=$!
    wait "$timer" && exec shutdown -r now
) </dev/null >/dev/null 2>&1 &
watchdog=$!
trap 'kill "$watchdog" 2>/dev/null || true; wait "$watchdog" 2>/dev/null || true' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
rc=0
echo
echo "== VCN before"
if ! "$C" status; then
    echo "Cannot query VCN status. Stopping before firmware staging."
    exit 1
fi
TMR="$TMR_ARG"
if [ -z "$TMR" ] && [ -e "$G/vcn-fw-at-boot" ]; then
    TMR=$(sed -n 's/.*fwtype 13 .*kr 0x00000000  status 0x00000000  .*fw_addr 0x\([0-9a-fA-F]\{16\}\)$/0x\1/p' "$G/fwaddr.log" 2>/dev/null | tail -1)
    [ "$TMR" != 0x0000000000000000 ] || TMR=""
    [ -n "$TMR" ] && echo "== VCN firmware preloaded at boot: $TMR"
    [ -z "$TMR" ] && echo "== vcn-fw-at-boot is set but fwaddr.log has no fwtype 13 line; loading through the PSP here"
fi
echo
echo "== VCN init ($FW)"
if [ -n "$TMR" ]; then INIT=( "$C" init --tmr "$TMR" "$FW" ); else INIT=( "$C" init "$FW" ); fi
if ! "${INIT[@]}"; then
    echo "init FAILED: powering VCN back down"
    "$C" down || echo "power down FAILED"
    rc=1
else
    echo
    echo "== ring test (VCN_ENC_CMD_END x4)"
    "$C" ring 4 || { echo "ring test FAILED"; rc=1; }
    if [ "$rc" = 0 ]; then
        echo
        echo "== fence test (VCN_ENC_CMD_FENCE x6, including ring wrap)"
        "$C" fence 6 || { echo "fence test FAILED"; rc=1; }
    fi
    if [ "$BUILD" -ge 60 ] && [ "$rc" = 0 ]; then
        for session in decode-session encode-session; do
            echo "== $session (create and close)"
            "$C" "$session" || { rc=1; break; }
        done
    fi
    if [ "$BUILD" -ge 62 ] && [ "$rc" = 0 ]; then
        "$C" decode-frame "$RUN_DIR/frame.nv12" || rc=1
        if [ "$rc" = 0 ]; then
            "$C" encode-frame "$RUN_DIR/frame.h264" && "$G/vcn_frame_check" "$RUN_DIR/frame.h264" || rc=1
        fi
    fi
    echo
    echo "== VCN after"
    "$C" status || true
    if [ "$KEEP" = 0 ] || [ "$rc" != 0 ]; then
        echo
        echo "== VCN power down (use --keep to leave it up)"
        "$C" down || { echo "power down FAILED"; rc=1; }
    fi
fi
echo
if [ "$rc" != 0 ]; then
    if [ "$BUILD" -ge 62 ]; then
        "$C" crumbs > "$RUN_DIR/crumbs.log" || true
    fi
    echo "RESULT: VCN bring-up failed (see PhxNativeABI.h for result codes)"
elif [ "$BUILD" -ge 62 ]; then
    echo "RESULT: H.264 VCN decode pixels and independently decoded encoder output passed; files: $RUN_DIR"
elif [ "$BUILD" -ge 60 ]; then
    echo "RESULT: VCN ring, fences and decoder/encoder session commands passed"
else
    echo "RESULT: VCN firmware, ring and fence test passed"
fi
exit $rc
