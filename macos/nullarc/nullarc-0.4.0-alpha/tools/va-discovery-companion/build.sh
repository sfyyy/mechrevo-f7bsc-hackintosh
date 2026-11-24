#!/bin/sh
# Build a local diagnostics bundle with the selected Command Line Tools SDK.
set -eu
cd "$(dirname "$0")"
sdk=$(xcrun --sdk macosx --show-sdk-path)
headers="$sdk/System/Library/Frameworks/Kernel.framework/Headers"
runtime=$(xcrun clang -print-resource-dir)/lib/darwin/libclang_rt.cc_kext.a
out=build/PhoenixVALoadProbe.kext
mkdir -p "$out/Contents/MacOS"
set -- -arch x86_64 -mmacosx-version-min=26.5 -isysroot "$sdk" -isystem "$headers" \
       -mkernel -DKERNEL -fno-builtin -fno-common -Os -Wall -Wextra -Werror \
       -Wno-unused-parameter -Wno-deprecated-declarations
xcrun clang "$@" -c kmod.c -o build/kmod.o
xcrun clang++ "$@" -std=c++17 -fapple-kext -fno-rtti -fno-exceptions \
    -c PhoenixVALoadProbe.cpp -o build/probe.o
xcrun clang++ -arch x86_64 -mmacosx-version-min=26.5 -isysroot "$sdk" \
    -nostdlib -Wl,-kext build/probe.o build/kmod.o -lkmod "$runtime" \
    -o "$out/Contents/MacOS/PhoenixVALoadProbe"
cp Info.plist "$out/Contents/Info.plist"
plutil -lint "$out/Contents/Info.plist"
codesign --force --sign "${PHX_CODESIGN_IDENTITY:--}" "$out"
codesign --verify --strict "$out"
