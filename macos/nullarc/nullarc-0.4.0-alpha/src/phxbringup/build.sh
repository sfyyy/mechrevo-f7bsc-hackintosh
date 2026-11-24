#!/bin/sh
# Builds PhxBringup.kext natively on macOS with the Command Line Tools.
# Usage: ./build.sh <path-to-MacKernelSDK>
set -eu
MKSDK=${1:?usage: build.sh <MacKernelSDK>}
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD=${PHX_BUILD_DIR:-"$HERE/build"}
OUT="$BUILD/PhxBringup.kext"
mkdir -p "$OUT/Contents/MacOS"

CFLAGS="-arch x86_64 -mmacosx-version-min=11.0 -mkernel -fno-builtin -fno-common -nostdinc
  -I$MKSDK/Headers -DKERNEL -DKERNEL_PRIVATE -DDRIVER_PRIVATE -DAPPLE -DNeXT -Os -Wall"

AVC="$HERE/../../tools/avc"
clang -x c $CFLAGS -c "$HERE/kmod_info.c" -o "$BUILD/kmod_info.o"
clang -x c $CFLAGS -std=c11 -c "$AVC/avd_h264.c" -o "$BUILD/avd_h264.o"
clang++ $CFLAGS -std=c++17 -fapple-kext -fno-rtti -fno-exceptions \
  -c "$AVC/avc_stream_parser.cpp" -o "$BUILD/avc_stream_parser.o"
clang++ $CFLAGS -std=c++17 -fapple-kext -fno-rtti -fno-exceptions \
  -c "$HERE/PhxBringup.cpp" -o "$BUILD/PhxBringup.o"
clang++ -arch x86_64 -mmacosx-version-min=11.0 -nostdlib -Xlinker -kext \
  -L"$MKSDK/Library/x86_64" -lkmod -lcc_kext \
  "$BUILD/kmod_info.o" "$BUILD/PhxBringup.o" "$BUILD/avd_h264.o" "$BUILD/avc_stream_parser.o" \
  -o "$OUT/Contents/MacOS/PhxBringup"
cp "$HERE/Info.plist" "$OUT/Contents/Info.plist"
echo "built $OUT"
