#!/bin/bash
# usage: nvmtl-env-no-mutation.sh [NVMTLDriver path]
# The default is the installed plugin. Use ~/gpu/nmplugin/NVMTLDriver.new
# to test an uninstalled build.
set -eu

HERE=$(cd "$(dirname "$0")"; pwd)
PLUGIN=${1:-/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver}
CONF=/Library/GPUBundles/nvmtl/nvrm610.conf

LINE=$(awk 'NF && $0 !~ /^[[:space:]]*#/ { print; exit }' "$CONF")
[ -n "$LINE" ] || { echo "no knob in $CONF" >&2; exit 1; }
KEY=${LINE%%=*}
VALUE=${LINE#*=}

PROBE=$(mktemp /tmp/nvmtl-env-no-mutation.XXXXXX)
trap 'unlink "$PROBE"' EXIT

xcrun clang -fobjc-arc -framework Foundation \
    "$HERE/nvmtl-env-no-mutation.m" -o "$PROBE"

env -i HOME="$HOME" TMPDIR="${TMPDIR:-/tmp}" PATH=/usr/bin:/bin \
    "$PROBE" "$PLUGIN" "$KEY" "$VALUE"
