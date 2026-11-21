#!/bin/bash
# Host tests for the hard-coded VCN bring-up. No hardware, no root: the register values are compared with the
# upstream Linux headers (tests/vcn_linux_regs.txt) and the firmware-header/layout logic is exercised directly.
#
#   src/phxbringup/tests/vcn_selftest.sh [vcn_4_0_0.bin]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
FW="${1:-/Users/Shared/gpu/fw/vcn_4_0_0.bin}"
[ -f "$FW" ] || echo "note: $FW is absent, the firmware-header test will be skipped"
python3 "$HERE/vcn_regs_check.py"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
clang++ -std=c++17 -Wall -Wextra -O1 -o "$TMP/vcn_host_test" "$HERE/vcn_host_test.cpp"
"$TMP/vcn_host_test" "$FW"
