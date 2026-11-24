#!/bin/bash
# Fence-trace the native path at each debug.phx_vcn_wait mode, so AC-2.1/2.2 can
# be compared per mode on one boot. Leaves debug.phx_vcn_wait=2 behind.
#   bash macos/nullarc/nullarc-0.4.0-alpha/tools/vcn-bench/trace-wait-modes.sh <output-dir> [clip]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root="$here"
out=${1:?usage: trace-wait-modes.sh <output-dir> [clip]}
clip=${2:-$root/results/build75-playback-audit.mUaoWr/gop60.h264}
mkdir -p "$out"
if [ ! -x /tmp/vt-metal ]; then
    xcrun clang -std=c11 -O2 "$root/results/build75-playback-audit.mUaoWr/vt-metal.m" \
        -framework Metal -framework VideoToolbox -framework CoreMedia \
        -framework CoreVideo -framework CoreFoundation -o /tmp/vt-metal
fi
for mode in 0 1 2; do
    sudo sysctl -w "debug.phx_vcn_wait=$mode" >/dev/null
    sleep 0.3
    python3 "$here/trace-native.py" --out "$out/wait$mode" --vt-metal /tmp/vt-metal "$clip:gop60"
done
sudo sysctl -w debug.phx_vcn_wait=2 >/dev/null
python3 - "$out" <<'PY'
import json, sys
base = sys.argv[1]
print("mode  frames  ioctl_p50  sleeps_p50  spin_p50_ms  groups")
for mode in (0, 1, 2):
    try:
        s = json.load(open(f"{base}/wait{mode}/native-gop60-fence-summary.json"))
    except FileNotFoundError:
        continue
    groups = {k: v["n"] for k, v in s.items() if k != "all"}
    print(f"{mode:>4}  {s['all']['n']:>6}  {s['all']['ioctl_ms']['p50']:>9.3f}  "
          f"{s['all']['gap_ms']['p50']:>10.3f}  -            {groups}")
PY
