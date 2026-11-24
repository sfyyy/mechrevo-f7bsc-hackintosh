#!/bin/bash
# Build the Intel CoreMedia encoder bundle for factory and discovery tests.
# The macOS 26.7.1 global registry requires an Apple-qualified signature.
set -eu
if [ "$#" -ne 1 ] || [ ! -d "$1" ]; then
    echo "usage: $0 EXISTING_OUTPUT_DIRECTORY" >&2
    exit 2
fi
src=$(cd "$(dirname "$0")" && pwd)
out=$(cd "$1" && pwd)
bundle="$out/PhoenixAVC.bundle"
if [ -e "$bundle" ]; then
    echo "$bundle already exists" >&2
    exit 1
fi
stage=$(mktemp -d "$out/.phoenix-vt.XXXXXXXX")
trap 'rm -rf "$stage"' EXIT
contents="$stage/PhoenixAVC.bundle/Contents"
mkdir -p "$contents/MacOS"
install -m 644 "$src/PhoenixAVC-Info.plist" "$contents/Info.plist"
plutil -lint "$contents/Info.plist" >/dev/null
xcrun clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror -bundle \
    -Wl,-exported_symbol,_PhxAVCEncoderCreate \
    "$src/phx_vt.c" "$src/phx_avc.c" "$src/phx_avc_io.c" \
    -framework VideoToolbox -framework CoreMedia -framework CoreVideo \
    -framework CoreFoundation -framework IOKit -o "$contents/MacOS/PhoenixAVC"
codesign --force --sign "${PHX_CODESIGN_IDENTITY:--}" "$stage/PhoenixAVC.bundle"
codesign --verify --strict "$stage/PhoenixAVC.bundle"
mv "$stage/PhoenixAVC.bundle" "$bundle"
echo "$bundle"
