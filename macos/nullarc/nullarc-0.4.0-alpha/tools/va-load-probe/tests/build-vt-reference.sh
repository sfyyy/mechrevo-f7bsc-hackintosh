#!/bin/bash
# Build only the process-local reference verifier. Invoke it with --project-reference.
set -euo pipefail
src=$(cd "$(dirname "$0")/.." && pwd)
transport="$src/phoenix_decode.c"
options=(-DPHX_PROJECT_VT_REFERENCE)
if [[ ${1:-} == --mock ]]; then
    shift
    transport="$src/tests/vt_reference_mock.c"
    options+=(-DPHX_TEST_SOFTWARE_TRANSPORT)
fi
if [[ $# != 1 || -e "$1" ]]; then
    echo "usage: $0 [--mock] <new verifier executable>" >&2
    exit 2
fi
xcrun clang -arch x86_64 -std=c11 -O2 -Wall -Wextra -Werror "${options[@]}" \
    -I"$src" -I"$src/../../src/phxaccel" \
    "$src/../verify-vt-decode.c" "$src/tests/vt_reference.c" "$transport" \
    -framework IOKit -framework CoreFoundation -framework CoreVideo -framework CoreMedia -framework VideoToolbox \
    -F"$(xcrun --show-sdk-path)/System/Library/PrivateFrameworks" -framework IOAccelerator -o "$1"
