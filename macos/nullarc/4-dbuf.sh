#!/bin/bash
# Rebuild PhxAccel.kext from nullarc v0.4.0-alpha with the double-buffered scan-out patch (patches/phxaccel-dbuf.patch)
# and install it into the Auxiliary kernel collection. Fixes tearing while dragging windows; see docs/known-issues.md.
# Needs Xcode Command Line Tools (xcode-select --install) and network access to github.com.
# Run in macOS, after 1-install.sh / 2-fix-15bf.sh / 3-enable-m2.sh:
#   sudo bash 4-dbuf.sh                 build, back up, install; then reboot
#   sudo bash 4-dbuf.sh --restore DIR   put the backed-up PhxAccel.kext back (DIR printed by the install); then reboot
# The patch is off until debug.phxfb_dbuf is set (macos/display/install.sh does that at every boot).
# Output: /Users/Shared/f7bsc-logs/phx-dbuf-<time>.txt
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
PATCH="$HERE/patches/phxaccel-dbuf.patch"
L=/Users/Shared/f7bsc-logs; mkdir -p "$L"
TS=$(date +%Y%m%d-%H%M%S)
OUT="$L/phx-dbuf-$TS.txt"
KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc
LE=/Library/Extensions/PhxAccel.kext
NULLARC_URL=https://github.com/luuumi-dev/nullarc/archive/refs/tags/v0.4.0-alpha.tar.gz
NULLARC_SHA=f624550ab768e6790ab4757edd3c81e716094857eae384af92ec4f40a710336a
MKSDK_URL=https://github.com/acidanthera/MacKernelSDK/archive/3f750085caa17ec3a7880f11c11bf4f48cd6a164.tar.gz
MKSDK_SHA=8931e622b6d16a7052cb2c2dabe16be06963a34b747f19e6cda2bbe8bc7e2557
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8

rebuild_kc() {
  local NEW=$KC.phx-new INS b; rm -f "$NEW"
  kmutil create -n aux --volume-root / --allow-missing-kdk --kernel /System/Library/Kernels/kernel \
    -B /System/Library/KernelCollections/BootKernelExtensions.kc -S /System/Library/KernelCollections/SystemKernelExtensions.kc \
    --repository /Library/Extensions --repository /Library/Apple/System/Library/Extensions -A "$NEW" -z
  echo "kmutil rc=$?"
  INS=$(kmutil inspect -a x86_64 -A "$NEW" 2>/dev/null)
  # every third-party kext of the current aux KC must be in the new one, or it is not published
  for b in $(kmutil inspect -a x86_64 -A "$KC" 2>/dev/null | awk 'NR>2 && $1 ~ /\./ {print $1}'); do
    echo "$INS" | grep -q "^$b[[:space:]]" && echo "  in KC: $b" || { echo "  MISSING: $b -- aux KC NOT replaced"; rm -f "$NEW"; return 1; }
  done
  mv -f "$NEW" "$KC" && echo "aux KC published"
}

if [ "${1:-}" = "--restore" ]; then
  B=${2:?usage: sudo bash 4-dbuf.sh --restore <backup dir>}
  [ -d "$B/PhxAccel.kext" ] || { echo "no $B/PhxAccel.kext"; exit 1; }
  ( set -x; rm -rf "$LE" && ditto "$B/PhxAccel.kext" "$LE" && chown -R root:wheel "$LE" && chmod -R go-w "$LE"; set +x
    rebuild_kc && echo "RESULT: OK, restored -- reboot now" ) > "$OUT" 2>&1
  grep -E 'RESULT|MISSING|kmutil rc' "$OUT"; echo "log: $OUT"; exit
fi

[ -f "$PATCH" ] || { echo "missing $PATCH"; exit 1; }
[ -d "$LE" ] || { echo "$LE missing: run 1-install.sh first"; exit 1; }
xcode-select -p >/dev/null 2>&1 || { echo "Command Line Tools missing: run xcode-select --install, then this again"; exit 1; }
W=$(mktemp -d /private/tmp/f7bsc-dbuf.XXXXXX)
BK=/Users/Shared/gpu/backup/phx-dbuf-$TS
(
set -x
cd "$W" || exit 1
curl -fsSL -o nullarc.tgz "$NULLARC_URL" && curl -fsSL -o mksdk.tgz "$MKSDK_URL" || { echo "download failed"; exit 1; }
echo "$NULLARC_SHA  nullarc.tgz" | shasum -a 256 -c || exit 1
echo "$MKSDK_SHA  mksdk.tgz" | shasum -a 256 -c || exit 1
mkdir src sdk && tar xzf nullarc.tgz -C src && tar xzf mksdk.tgz -C sdk || exit 1
SRC=$(echo "$W"/src/nullarc-*) SDK=$(echo "$W"/sdk/MacKernelSDK-*)
( cd "$SRC" && patch -p1 < "$PATCH" ) || { echo "patch did not apply"; exit 1; }
sh "$SRC/src/phxaccel/build.sh" "$SDK" || { echo "build failed"; exit 1; }
NEWK="$SRC/src/phxaccel/build/PhxAccel.kext"
strings "$NEWK/Contents/MacOS/PhxAccel" | grep -q phxScanoutBack || { echo "built kext lacks the patch"; exit 1; }
mkdir -p "$BK" && cp -p "$KC" "$BK/" && ditto "$LE" "$BK/PhxAccel.kext" && echo "backup: $BK"
rm -rf "$LE" && ditto "$NEWK" "$LE"
cp "$BK/PhxAccel.kext/Contents/Info.plist" "$LE/Contents/Info.plist"   # keep the installed personalities (2-fix-15bf / 3-enable-m2)
xattr -cr "$LE"; chown -R root:wheel "$LE"; chmod -R go-w "$LE"
codesign --force --sign - "$LE" && codesign --verify --strict "$LE" || exit 1
set +x
echo "===== rebuild Auxiliary kernel collection"
if rebuild_kc; then
  echo "RESULT: OK, reboot now. Undo: sudo bash $0 --restore $BK"
else
  echo "putting the previous kext back"; rm -rf "$LE"; ditto "$BK/PhxAccel.kext" "$LE"; chown -R root:wheel "$LE"
  echo "RESULT: FAILED, nothing changed"; exit 1
fi
) > "$OUT" 2>&1
RC=$?
rm -rf "$W"
sync
grep -E 'RESULT|backup:|MISSING|kmutil rc|failed|did not apply|lacks|FAILED|error' "$OUT"
echo "log: $OUT"
exit $RC
