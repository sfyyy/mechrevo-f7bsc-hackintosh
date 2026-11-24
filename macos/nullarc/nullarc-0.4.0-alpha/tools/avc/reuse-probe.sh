#!/bin/bash
# Verifies on real streams the property the IDR session reuse depends on: every
# IDR carries an empty reference list with frame_num 0 and slot 0, and the
# firmware message for it builds. usage: reuse-probe.sh <clip.h264>...
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d "${TMPDIR:-/tmp}/phx-reuse-probe.XXXXXXXX")
trap 'rm -rf "$tmp"' EXIT
"${CC:-clang}" -std=c11 -O1 -g -Wall -Wextra -c "$here/avd_h264.c" -o "$tmp/avd.o"
"${CXX:-clang++}" -std=c++17 -O1 -g -Wall -Wextra -Werror \
    "$here/avc_stream_parser.cpp" "$here/avc_reuse_probe.cpp" "$tmp/avd.o" -o "$tmp/reuse-probe"
exec "$tmp/reuse-probe" "$@"
