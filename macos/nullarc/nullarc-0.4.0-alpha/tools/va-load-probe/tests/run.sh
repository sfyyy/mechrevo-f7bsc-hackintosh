#!/bin/bash
# Run with the Phoenix VCN already initialized. No system installation is used.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
base=$(cd "$here/.." && pwd)
vcn="$base/../vcn"
run=$(mktemp -d "${TMPDIR:-/tmp}/va-callbacks-XXXXXXXX")
echo "$run"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
bundle="$build/PhoenixVALoadProbe.bundle"
bash "$base/build.sh" "$bundle"
xcrun clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror "$here/callbacks.c" \
    -framework CoreFoundation -framework CoreVideo -framework IOSurface -framework IOKit -o "$build/callbacks"
xcrun clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror "$vcn/vcn_frame_check.c" \
    -framework CoreFoundation -framework CoreMedia -framework CoreVideo -framework VideoToolbox -o "$build/check"
executable="$bundle/Contents/MacOS/PhoenixVALoadProbe"
shasum -a 256 "$executable" > "$run/bundle-sha256.txt"
"$build/callbacks" "$executable" "$run/hardware.h264" "$run/input.nv12" 2>&1 | tee "$run/callbacks.log"
"$build/check" "$run/hardware.h264" "$run/input.nv12" 2>&1 | tee "$run/decode.log"
shasum -a 256 "$run/hardware.h264" > "$run/stream-sha256.txt"
