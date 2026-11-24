#!/bin/bash
# One command for the acceptance window after installing build 78 and rebooting:
# checks the build, runs the A/B scan, compares every frame against the software
# decoder, and leaves one results directory behind.
#
#   sudo bash macos/nullarc/nullarc-0.4.0-alpha/install/vcn/install-vcn.sh && sudo shutdown -r now
#   bash macos/nullarc/nullarc-0.4.0-alpha/tools/vcn-bench/postboot.sh [tag]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root="$here"
tag=${1:-$(date +%Y%m%d-%H%M%S)}
out="$root/results/decode-perf-acceptance.$tag"
mkdir -p "$out"

build=$(ioreg -r -c PhxBringup -d 1 | sed -n 's/.*"phx-build" = \([0-9][0-9]*\).*/\1/p' | head -1)
echo "== phx-build = ${build:-missing}"
if [ "${1:-}" != "--keep-build" ] && [ "$build" != "78" ]; then
    echo "expected build 78; install it first (sudo bash macos/nullarc/nullarc-0.4.0-alpha/install/vcn/install-vcn.sh) and reboot" >&2
    exit 1
fi

echo "== A/B scan"
python3 "$here/ab-scan.py" "$out/ab"

echo "== per-frame pixel comparison (hardware vs software)"
python3 "$here/pixel-compare.py" \
    "$root/results/build71-cpu-compare.GoFC6LIS/repeated-1080p.h264" \
    "$root/results/build75-playback-audit.mUaoWr/gop60.h264" \
    "$root/results/vcn-build73-chrome/douyu-540.h264" \
    "$root/results/vcn-build73-chrome/douyu-1080.h264" \
    "$root/results/regression-streams/geometry-switch.h264" \
    | tee "$out/pixels.txt"

echo "== AVC session rebuilds (AC-3: the steady state must not appear here)"
# IOLog lands in the kernel message buffer, not the unified log.
sudo -n dmesg 2>/dev/null | grep -o 'reason=[a-z-]* [0-9]*x[0-9]*' | sort | uniq -c | tee "$out/rebuilds.txt"

echo "== registry snapshot"
ioreg -r -c PhxBringup -d 1 | grep -E 'phx-build|PhxAvcCounters|PhxAvcLastTiming|PhxAvcReadbackMode' \
    | tee "$out/counters.txt"

echo "written $out"
