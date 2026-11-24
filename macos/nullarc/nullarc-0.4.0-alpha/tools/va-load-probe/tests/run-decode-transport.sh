#!/bin/bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
if [[ $# == 2 && $1 == --mock-stream ]]; then
    mock=1
elif [[ $# == 3 && ( $1 == --mock || $1 == --hardware ) ]]; then
    mock=0
    if [[ $1 == --mock ]]; then mock=1; fi
else
    echo "Usage: $0 --mock|--hardware libdrm-idr.h264 libdrm-output.nv12" >&2
    echo "       $0 --mock-stream path/to/1920x1080-fixture-directory" >&2
    exit 2
fi
shift
build=$(mktemp -d)
trap 'rm -rf "$build"' EXIT
xcrun clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror \
    "-DPHX_TEST_MOCK=$mock" -I"$here/.." -I"$here/../../../src/phxaccel" \
    "$here/decode-transport.c" "$here/../phoenix_decode.c" \
    -framework IOKit -framework CoreFoundation -framework CoreMedia -framework CoreVideo \
    -F"$(xcrun --show-sdk-path)/System/Library/PrivateFrameworks" -framework IOAccelerator -o "$build/transport"
"$build/transport" "$@"
