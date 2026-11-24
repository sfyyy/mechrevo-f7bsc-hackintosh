#!/bin/bash
set -euo pipefail
if [[ $# != 1 && ! ( $# == 4 && $1 == --hardware ) ]]; then
    echo "Usage: bash $0 libdrm-idr.h264 | --hardware PhoenixVALoadProbe.bundle libdrm-idr.h264 libdrm-output.nv12" >&2
    exit 2
fi
src=$(cd "$(dirname "$0")/.." && pwd)
avc="$src/../avc"
vcn="$src/../vcn"
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
if [[ $# == 4 ]]; then
    xcrun clang -DPHX_TEST_BUNDLE=1 -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror \
        -I"$src" -I"$src/../../src/phxaccel" -I"$vcn" \
        "$src/tests/decode-callbacks.c" -framework IOKit -framework IOSurface \
        -framework CoreFoundation -framework CoreVideo -o "$build/decode-callbacks"
    "$build/decode-callbacks" "$3" "$2/Contents/MacOS/PhoenixVALoadProbe" "$4"
    exit
fi
for mode in release probe; do
    probe=0
    if [[ $mode == probe ]]; then probe=1; fi
    xcrun clang -DPHX_TEST_BUNDLE=0 "-DPHX_REFERENCE_DECODE_PROBE=$probe" -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror \
        -I"$src" -I"$src/../../src/phxaccel" -I"$vcn" \
        -I"$avc" "$src/tests/decode-callbacks.c" "$src"/avd_*.c "$src/phoenix_encode.c" \
        "$src/avc_parameters.c" "$avc/avd_h264.c" \
        "$vcn/phx_avc.c" "$vcn/phx_avc_io.c" -framework IOKit -framework IOSurface \
        -framework CoreFoundation -framework CoreMedia -framework CoreVideo \
        -F"$(xcrun --show-sdk-path)/System/Library/PrivateFrameworks" -framework IOAccelerator \
        -o "$build/decode-callbacks"
    "$build/decode-callbacks" "$1" "$mode"
done
