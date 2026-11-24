#!/bin/bash
# AC-7.1 long run: decode in chunks so the counters can be sampled between them.
# Fails on decode errors, on max_fence_us above 50 ms, or on a lost build.
#
#   bash macos/nullarc/nullarc-0.4.0-alpha/tools/vcn-bench/long-run.sh 2            # two hours on the 1080p clip
#   bash macos/nullarc/nullarc-0.4.0-alpha/tools/vcn-bench/long-run.sh 0.1 B        # six minute smoke
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root="$here"
hours=${1:?usage: long-run.sh <hours> [A|B] [chunk-loops]}
which=${2:-B}
loops=${3:-10}
case "$which" in
    A) clip="$root/results/build71-cpu-compare.GoFC6LIS/repeated-1080p.h264" ;;
    *) clip="$root/results/build75-playback-audit.mUaoWr/gop60.h264" ;;
esac
bin=/tmp/vt-metal
if [ ! -x "$bin" ]; then
    xcrun clang -std=c11 -O2 "$root/results/build75-playback-audit.mUaoWr/vt-metal.m" \
        -framework Metal -framework VideoToolbox -framework CoreMedia \
        -framework CoreVideo -framework CoreFoundation -o "$bin"
fi
out="$root/results/long-run.$(date +%Y%m%d-%H%M%S)"
mkdir -p "$out"
csv="$out/samples.csv"
echo "t,seconds,errors,fps,cpu_ms,counters" > "$csv"
span=$(awk -v h="$hours" 'BEGIN { printf "%d", h * 3600 }')
start=$(date +%s)
deadline=$(( start + span ))
echo "start maybe-long run: $(date -u +%FT%TZ) clip=$which chunk=$loops loops => $out"
failures=0
while [ $(date +%s) -lt "$deadline" ]; do
    json=$("$bin" --hardware "$clip" "$loops" 2>/dev/null | tail -1)
    fps=$(printf '%s' "$json" | sed -n 's/.*"fps":\([0-9.]*\).*/\1/p')
    errors=$(printf '%s' "$json" | sed -n 's/.*"errors":\([0-9]*\).*/\1/p')
    cpu=$(printf '%s' "$json" | sed -n 's/.*"cpu_ms_per_frame":\([0-9.]*\).*/\1/p')
    counters=$(ioreg -r -c PhxBringup -d 1 | sed -n 's/.*"PhxAvcCounters" = "\([^"]*\)".*/\1/p' | head -1)
    now=$(date +%s)
    echo "$(date +%H:%M:%S),$(( now - start )),${errors:-?},${fps:-?},${cpu:-?},\"$counters\"" >> "$csv"
    printf '%s errors=%s fps=%s cpu_ms=%s %s\n' "$(date +%H:%M:%S)" "${errors:-?}" "${fps:-?}" "${cpu:-?}" "$counters"
    if [ "${errors:-1}" != "0" ]; then
        echo "decode errors reported: ${errors}" >&2
        failures=$(( failures + 1 ))
    fi
    maxf=$(printf '%s' "$counters" | sed -n 's/.*max_fence_us=\([0-9]*\).*/\1/p')
    if [ -n "${maxf}" ] && [ "${maxf}" -ge 50000 ]; then
        echo "max_fence_us=$maxf is above the 50 ms budget" >&2
        failures=$(( failures + 1 ))
    fi
done
echo "done: $failures failure(s); samples in $csv"
exit $(( failures ? 1 : 0 ))
