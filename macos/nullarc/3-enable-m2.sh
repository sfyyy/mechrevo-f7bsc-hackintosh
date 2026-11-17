#!/bin/bash
# Put WindowServer on the 780M (nullarc "M2": the GPU composites the desktop, which brings back the wallpaper, glass
# and blur) on the F7BSC mini PC, which has no internal panel.
# PhxAMDFB takes over the mode the firmware GOP lit and writes nothing to the display engine (src/phxaccel/accel/
# phxfb.cpp): on this machine that mode is the HDMI monitor. The release's PhxFB personality matches 0x19001002 only;
# this script extends it to the upstream list, as phx-fix-15bf.sh did for the accelerator, and arms M2 the way
# `install.sh --windowserver` does (the marker /Users/Shared/gpu/phxboot-m2).
# Run in macOS:  sudo bash 3-enable-m2.sh   (after 2-fix-15bf.sh and a reboot)
# Output: /Users/Shared/f7bsc-logs/phx-enable-m2-<time>.txt
# Undo:   sudo rm /Users/Shared/gpu/phxboot-m2; sudo nvram -d phxfb-boot   (then reboot)
L=/Users/Shared/f7bsc-logs; mkdir -p "$L"
G=/Users/Shared/gpu
K=/Library/Extensions/PhxAccel.kext
P=$K/Contents/Info.plist
MATCH="0x19001002 0x15bf1002 0x15c81002 0x164e1002"
TS=$(date +%Y%m%d-%H%M%S)
OUT="$L/phx-enable-m2-$TS.txt"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
(
echo "===== preconditions"
ioreg -rc NMXAccel -d0 | grep -q NMXAccel || { echo "STOP: NMXAccel is not running (run phx-fix-15bf.sh and reboot first)"; exit 1; }
echo "NMXAccel running"
last=$(grep -v '^$' /var/log/phxboot.log 2>/dev/null | tail -1)
case "$last" in done*) echo "phxboot.log: $last" ;; *) echo "STOP: last phxboot.log line is '$last', not 'done'"; exit 1 ;; esac
[ -e $G/phxboot-off ] && { echo "STOP: $G/phxboot-off exists (GPU turned off)"; exit 1; }
echo "FileVault: $(fdesetup status 2>&1)"
AUTO=$(defaults read /Library/Preferences/com.apple.loginwindow autoLoginUser 2>/dev/null)
if [ -n "$AUTO" ] && [ -f /etc/kcpassword ]; then echo "automatic login: $AUTO"
else echo "WARNING: automatic login is OFF. Turn it on (System Settings -> Users & Groups) before the second reboot:"
     echo "         the login window may come up black on the GPU path."; fi

echo "===== PhxFB personality -> $MATCH"
set -x
ditto "$K" "$G/backup/PhxAccel.kext.$TS"
plutil -remove IOKitPersonalities.PhxFB.IOPCIPrimaryMatch "$P" 2>/dev/null
plutil -replace IOKitPersonalities.PhxFB.IOPCIMatch -string "$MATCH" "$P" || exit 1
plutil -remove IOKitPersonalities.PhxAccel.IOPCIPrimaryMatch "$P" 2>/dev/null
plutil -replace IOKitPersonalities.PhxAccel.IOPCIMatch -string "$MATCH" "$P" || exit 1
plutil -lint "$P" || exit 1
plutil -extract IOKitPersonalities.PhxFB.IOPCIMatch raw -o - "$P"
plutil -extract IOKitPersonalities.PhxAccel.IOPCIMatch raw -o - "$P"
xattr -cr "$K"; chown -R root:wheel "$K"; chmod -R go-w "$K"
codesign --force --sign - "$K" || exit 1
codesign --verify --strict --verbose=2 "$K" || exit 1
set +x
echo "===== rebuild Auxiliary kernel collection"
BK=$G/backup $G/kinst.sh || { echo "kinst exit: failed, aux KC not published"; exit 1; }
echo "kinst exit: 0"
kmutil inspect -a x86_64 -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>/dev/null | grep -i luuumi

echo "===== arm M2"
touch $G/phxboot-m2; chown root:wheel $G/phxboot-m2
ls -la $G/phxboot-m2
nvram phxfb-boot 2>&1
echo "RESULT: OK. Reboot 1 stays on the firmware framebuffer and arms PhxAMDFB at its end;"
echo "        reboot 2 composites the desktop on the 780M."
) > "$OUT" 2>&1
RC=$?
sync
echo "log: $OUT"
grep -E 'RESULT|STOP|WARNING|automatic login|FileVault|valid on disk|kinst exit|luuumi' "$OUT"
exit $RC
