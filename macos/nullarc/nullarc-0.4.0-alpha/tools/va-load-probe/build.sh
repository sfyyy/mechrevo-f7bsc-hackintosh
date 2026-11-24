#!/bin/bash
set -euo pipefail

src=$(cd "$(dirname "$0")" && pwd)
transport="$src/phoenix_encode.c"
accel="$src/../../src/phxaccel"
avc="$src/../avc"
reference_decode=0
if [[ ${1:-} == --video-context-probe ]]; then
    shift
    transport="$src/tests/video_context.c"
elif [[ ${1:-} == --reference-decode-probe ]]; then
    shift
    reference_decode=1
fi
if [[ $# != 1 || -e "$1" ]]; then
    echo "Usage: bash build.sh [--video-context-probe|--reference-decode-probe] <new PhoenixVALoadProbe.bundle path>" >&2
    exit 2
fi
vcn="$src/../vcn"
abi="$src/../../src/phxbringup"   # PhxVcnUserABI.h
stage=$(mktemp -d "$(dirname "$1")/.phoenix-va.XXXXXXXX")
trap 'rm -rf "$stage"' EXIT
bundle="$stage/PhoenixVALoadProbe.bundle"
mkdir -p "$bundle/Contents/MacOS"
cp "$src/Info.plist" "$bundle/Contents/Info.plist"
xcrun clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -fvisibility=hidden \
    -bundle "-DPHX_REFERENCE_DECODE_PROBE=$reference_decode" -I"$accel" -I"$vcn" -I"$abi" -I"$src" -I"$avc" \
    "$src/factory.c" "$src"/avd_*.c "$src/avc_parameters.c" "$avc/avd_h264.c" "$transport" "$src/phoenix_decode.c" \
    "$vcn/phx_avc.c" "$vcn/phx_avc_io.c" -framework IOKit -framework IOSurface \
    -framework CoreFoundation -framework CoreMedia -framework CoreVideo \
    -F"$(xcrun --show-sdk-path)/System/Library/PrivateFrameworks" -framework IOAccelerator \
    -o "$bundle/Contents/MacOS/PhoenixVALoadProbe"
codesign --force --sign - "$bundle"
codesign --verify --strict "$bundle"
mv "$bundle" "$1"
