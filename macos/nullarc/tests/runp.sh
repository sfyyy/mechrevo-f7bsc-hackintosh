#!/bin/bash
# usage: runp.sh DRIVER cmd... — run a test app against an UNINSTALLED plugin build. No sudo; WindowServer untouched.
#   e.g. runp.sh ~/nullarc/src/nmplugin/NVMTLDriver.new ./carender 600 40 4
# First run: copies the installed NVMTLDriver.bundle to ./drv (the translator and air-runtime must sit next to the
# executable) and builds shim/redirect.dylib. DRIVER is copied into that bundle under a unique name, and the shim makes
# Metal load it in place of the installed executable (dlopen redirect + class_getImageName / objc_copyClassNamesForImage
# answers, which Metal checks). Re-copy ./drv after installing a new translator.
set -e
T=$(cd "$(dirname "$0")"; pwd); D=$1; shift
B=/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU/NVMTLDriver.bundle
[ -d "$T/drv/NVMTLDriver.bundle" ] || { mkdir -p "$T/drv"; rsync -a "$B" "$T/drv/"; }
[ -f "$T/shim/redirect.dylib" ] || xcrun clang -arch x86_64 -dynamiclib -O2 -o "$T/shim/redirect.dylib" "$T/shim/redirect.c" -lobjc
X=$T/drv/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver.$(shasum "$D" | cut -c1-10)
[ -f "$X" ] || cp "$D" "$X"
NVMTL_REDIRECT=$X DYLD_INSERT_LIBRARIES=$T/shim/redirect.dylib exec "$@"
