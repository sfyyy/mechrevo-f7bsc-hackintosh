#!/bin/bash
# Let NMXAccel (PhxAccel.kext's accelerator) attach to this machine's Radeon 780M, PCI 1002:15bf.
# The nullarc 0.4.0-alpha release ships PhxAccel with IOPCIPrimaryMatch 0x19001002 only (H 255); upstream source
# (luuumi-dev/nullarc 4459c408, 2026-11-09) matches 0x19001002 0x15bf1002 0x15c81002 0x164e1002. Only the PhxAccel
# personality is changed: the PhxFB (PhxAMDFB, panel framebuffer) personality stays 0x1900-only, so the firmware
# framebuffer keeps driving the HDMI display on this panel-less mini PC.
# Run in macOS:  sudo bash 2-fix-15bf.sh   (after 1-install.sh; reboot afterwards)
# Output: /Users/Shared/f7bsc-logs/phx-fix-15bf-<time>.txt
L=/Users/Shared/f7bsc-logs; mkdir -p "$L"
K=/Library/Extensions/PhxAccel.kext
P=$K/Contents/Info.plist
MATCH="0x19001002 0x15bf1002 0x15c81002 0x164e1002"
TS=$(date +%Y%m%d-%H%M%S)
BK=/Users/Shared/gpu/backup/PhxAccel.kext.$TS
OUT="$L/phx-fix-15bf-$TS.txt"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
(
set -x
[ -f "$P" ] || { echo "$P missing: run the installer first"; exit 1; }
ditto "$K" "$BK" && echo "backup: $BK"
plutil -extract IOKitPersonalities.PhxAccel xml1 -o - "$P"
plutil -remove IOKitPersonalities.PhxAccel.IOPCIPrimaryMatch "$P" 2>/dev/null
plutil -replace IOKitPersonalities.PhxAccel.IOPCIMatch -string "$MATCH" "$P" || exit 1
plutil -lint "$P" || exit 1
echo "--- after"
plutil -extract IOKitPersonalities.PhxAccel xml1 -o - "$P"
plutil -extract IOKitPersonalities.PhxFB.IOPCIPrimaryMatch raw -o - "$P"
xattr -cr "$K"
chown -R root:wheel "$K"; chmod -R go-w "$K"
codesign --force --sign - "$K" || exit 1
codesign --verify --strict --verbose=2 "$K" || exit 1
set +x
echo "===== rebuild Auxiliary kernel collection"
BK=/Users/Shared/gpu/backup /Users/Shared/gpu/kinst.sh || { echo "kinst exit: failed, aux KC not published"; exit 1; }
echo "kinst exit: 0"
kmutil inspect -a x86_64 -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>/dev/null | grep -i luuumi
echo "===== state before reboot"
ls -la /Users/Shared/gpu/phxboot-off /Users/Shared/gpu/phxboot-m2 /var/db/phxboot.pending 2>&1
nvram phxfb-boot 2>&1
echo "RESULT: OK, reboot now"
) > "$OUT" 2>&1
RC=$?
sync
echo "log: $OUT"
grep -E 'RESULT|valid on disk|satisfies|kinst exit|luuumi|missing|Error|error' "$OUT"
exit $RC
