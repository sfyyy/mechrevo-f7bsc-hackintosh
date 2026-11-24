#!/bin/bash
# Builds and runs the direct-write layout test. No device, no installation.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
xcrun clang -arch "$(uname -m)" -std=c11 -O2 -Wall -Wextra -Werror \
    "$here/direct_layout_test.c" \
    -framework CoreFoundation -framework CoreVideo -framework IOSurface \
    -o "$build/direct_layout_test"
exec "$build/direct_layout_test"
