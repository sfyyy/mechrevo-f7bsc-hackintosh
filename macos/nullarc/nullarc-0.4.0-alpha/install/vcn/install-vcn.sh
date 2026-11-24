#!/bin/bash
# Install the PhxBringup build that carries the hard-coded VCN 4.0.2 bring-up (selector 120) and the vcnctl tool
# that drives it. The VCN code never runs at boot: until vcnctl asks for it, the kext behaves exactly like the
# build it replaces.
#
#   sudo bash install-vcn.sh            # detect the OpenCore ESP, back it up, replace the kext, install vcnctl+fw
#   sudo bash install-vcn.sh --dry-run  # only run the checks and report
#   sudo bash install-vcn.sh --tool-only  # update vcnctl without touching the ESP
#
# Takes effect at the next boot. Undo: sudo bash rollback-vcn.sh
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
G=/Users/Shared/gpu
# the kext to install: the one src/phxbringup/build.sh produces, or KEXT=<path>
SRC="${KEXT:-$REPO/src/phxbringup/build/PhxBringup.kext}"
FW="$G/fw/vcn_4_0_0.bin"
FW_SHA=5633a5212d41e1f3932c1e7e630b1f2c8c4d27bc53ce8149c3be75d1d832053c
FW_URL=https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain/amdgpu/vcn_4_0_0.bin
DRY=0
ESP_ARG=""

say() { echo "== $*"; }
die() { echo "ERROR: $*" >&2; exit 1; }

check_graphics_health() {
    local loaded display_data
    [ ! -e "$G/phxboot-off" ] || die "GPU kill switch exists at $G/phxboot-off; restore desktop acceleration and reboot first"
    [ ! -e /var/db/phxboot.pending ] || die "phxboot has an unfinished startup attempt; resolve it and reboot before installing"
    [ -e "$G/phxboot-m2" ] || die "WindowServer GPU compositing is not armed ($G/phxboot-m2 is missing)"
    nvram -p | awk '$1 == "phxfb-boot" && $2 == "1" { found=1 } END { exit !found }' ||
        die "NVRAM phxfb-boot is not armed; restore desktop acceleration and reboot first"
    [ -f /var/log/phxboot.log ] && tail -n 1 /var/log/phxboot.log | grep -q '^done ' ||
        die "the latest phxboot run did not finish successfully"
    ioreg -rc PhxAMDFB -d 0 | grep -q 'class PhxAMDFB' ||
        die "PhxAMDFB is not active; refusing to replace the graphics bring-up kext"
    sysctl -n debug.nvaccel_iop_ok 2>/dev/null | awk '$1 > 0 { found=1 } END { exit !found }' ||
        die "the GPU has not completed a desktop frame; refusing to replace the graphics bring-up kext"
    loaded=$(kmutil showloaded --list-only 2>/dev/null) || die "cannot inspect loaded kernel extensions"
    printf '%s\n' "$loaded" | grep -q 'org.drip.PhxBringup' || die "PhxBringup is not loaded"
    printf '%s\n' "$loaded" | grep -q 'com.luuumi.PhxAccel' || die "PhxAccel is not loaded"
    display_data=$(system_profiler SPDisplaysDataType -json 2>/dev/null) ||
        die "cannot inspect Metal support"
    printf '%s\n' "$display_data" | python3 -c '
import json, sys
try:
    data = json.load(sys.stdin)
    supported = any(
        display.get("spdisplays_mtlgpufamilysupport") == "spdisplays_metal3"
        for item in data.get("SPDisplaysDataType", [])
        for display in [item]
    )
except (ValueError, TypeError):
    supported = False
sys.exit(0 if supported else 1)
' || die "System Information does not report Metal 3; refusing to replace the graphics bring-up kext"
    say "graphics health verified: PhxBringup, PhxAccel, PhxAMDFB and Metal 3"
}

install_tool() (
    say "building vcnctl"
    tool=$(mktemp "$G/.vcnctl.XXXXXX")
    checker=$(mktemp "$G/.vcn-frame-check.XXXXXX")
    trap 'rm -f "$tool" "$checker"' EXIT
    clang -O2 -Wall -Wextra -Werror -o "$tool" "$REPO/tools/phxctl/vcnctl.c" \
        -framework IOKit -framework CoreFoundation
    clang -std=c11 -O2 -Wall -Wextra -Werror -o "$checker" "$REPO/tools/phxctl/vcn_frame_check.c" \
        -framework VideoToolbox -framework CoreMedia -framework CoreVideo -framework CoreFoundation
    chmod 755 "$tool" "$checker"
    mv -f "$checker" "$G/vcn_frame_check"
    mv -f "$tool" "$G/vcnctl"
    say "installed $G/vcnctl"
)

case "${1:-}" in
    --tool-only) [ "$#" = 1 ] || die "--tool-only takes no arguments"; install_tool; exit 0 ;;
    --dry-run) [ "$#" = 1 ] || die "--dry-run takes no arguments"; DRY=1 ;;
    --esp) [ "$#" = 2 ] && [ -n "$2" ] || die "--esp requires a disk identifier"; ESP_ARG="$2" ;;
    "") ;;
    *) die "unknown argument: $1" ;;
esac

[ -d "$SRC" ] || die "missing $SRC (build it with src/phxbringup/build.sh <MacKernelSDK>, or pass KEXT=<path>)"
[ -f "$SRC/Contents/MacOS/PhxBringup" ] || die "$SRC has no executable"
plutil -lint "$SRC/Contents/Info.plist" >/dev/null || die "Info.plist in $SRC is invalid"
BID=$(plutil -extract CFBundleIdentifier raw -o - "$SRC/Contents/Info.plist")
[ "$BID" = "org.drip.PhxBringup" ] || die "unexpected bundle identifier $BID"
file "$SRC/Contents/MacOS/PhxBringup" | grep -q 'kext bundle x86_64' || die "not an x86_64 kext binary"
say "candidate kext: $SRC ($(shasum -a 256 "$SRC/Contents/MacOS/PhxBringup" | cut -c1-16)...)"
check_graphics_health
say "checking kernel-visible dependencies"
DIAG=$(kmutil print-diagnostics --bundle-path "$SRC" 2>&1 || true)
echo "$DIAG" | grep -q 'Dependencies: OK' || { echo "$DIAG"; die "kmutil could not resolve the kext dependencies"; }

# --- firmware ---------------------------------------------------------------------------------------------------
if [ ! -f "$FW" ] || [ "$(shasum -a 256 "$FW" | awk '{print $1}')" != "$FW_SHA" ]; then
    LOCAL="${VCN_FW:-$REPO/vcn-ref/vcn_4_0_0.bin}"
    if [ -f "$LOCAL" ] && [ "$(shasum -a 256 "$LOCAL" | awk '{print $1}')" = "$FW_SHA" ]; then
        say "installing firmware from $LOCAL"
        [ $DRY = 1 ] || { mkdir -p "$G/fw"; cp -p "$LOCAL" "$FW"; }
    else
        say "downloading VCN 4.0.2 firmware (linux-firmware)"
        [ $DRY = 1 ] || {
            mkdir -p "$G/fw"
            curl -fsSL --max-time 120 -o "$FW.new" "$FW_URL" || die "download failed; get amdgpu/vcn_4_0_0.bin from linux-firmware"
            [ "$(shasum -a 256 "$FW.new" | awk '{print $1}')" = "$FW_SHA" ] || { rm -f "$FW.new"; die "firmware checksum mismatch"; }
            mv -f "$FW.new" "$FW"
        }
    fi
fi
[ $DRY = 1 ] || [ "$(shasum -a 256 "$FW" | awk '{print $1}')" = "$FW_SHA" ] || die "firmware at $FW has the wrong sha256"

# --- vcnctl -----------------------------------------------------------------------------------------------------
[ "$DRY" = 1 ] || install_tool

# --- find the OpenCore ESP --------------------------------------------------------------------------------------
CAND=$(python3 - <<'PY'
import plistlib, subprocess
d = plistlib.loads(subprocess.check_output(["diskutil", "list", "-plist"]))
for disk in d.get("AllDisksAndPartitions", []):
    for p in disk.get("Partitions", []):
        if p.get("Content") == "EFI" or str(p.get("Content", "")).startswith("EFI"):
            print(p["DeviceIdentifier"])
PY
) || die "cannot enumerate disks (diskutil list -plist failed)"
[ -n "$CAND" ] || die "no EFI partition found"
mount_point() {
    python3 - "$1" <<'PY'
import plistlib, subprocess, sys
d = plistlib.loads(subprocess.check_output(["diskutil", "info", "-plist", "/dev/" + sys.argv[1]]))
print(d.get("MountPoint") or "")
PY
}
TARGET_ID=""; TARGET_MP=""
for id in $CAND; do
    if [ -n "$ESP_ARG" ] && [ "$id" != "$ESP_ARG" ]; then continue; fi
    was_mounted=$(mount_point "$id" || true)
    if [ -z "$was_mounted" ]; then
        diskutil mount "/dev/$id" >/dev/null 2>&1 || continue
        mp=$(mount_point "$id" || true)
    else
        mp="$was_mounted"
    fi
    if [ -d "$mp/EFI/OC/Kexts/PhxBringup.kext" ] && plutil -p "$mp/EFI/OC/config.plist" 2>/dev/null | grep -q PhxBringup; then
        if [ -n "$TARGET_ID" ]; then die "more than one OpenCore ESP: $TARGET_ID and $id; rerun with --esp $id"; fi
        TARGET_ID=$id; TARGET_MP=$mp
    elif [ -z "$was_mounted" ]; then
        diskutil unmount "/dev/$id" >/dev/null 2>&1 || true
    fi
done
[ -n "$TARGET_ID" ] || die "no ESP with EFI/OC/Kexts/PhxBringup.kext; pass --esp diskXsY"
DEST="$TARGET_MP/EFI/OC/Kexts/PhxBringup.kext"
say "OpenCore ESP: /dev/$TARGET_ID mounted at $TARGET_MP"
say "replacing $DEST"
python3 - "$DEST/Contents/Info.plist" "$SRC/Contents/Info.plist" <<'PY' || die "candidate kext changes graphics matching metadata"
import plistlib, sys
keys = ("CFBundleIdentifier", "IOClass", "IOMatchCategory", "IOProviderClass",
        "IOUserClientClass", "IOPCIMatch")
def personality(path):
    with open(path, "rb") as stream:
        plist = plistlib.load(stream)
    return (plist["CFBundleIdentifier"],
            {key: plist["IOKitPersonalities"]["PhxBringup"].get(key) for key in keys[1:]})
sys.exit(0 if personality(sys.argv[1]) == personality(sys.argv[2]) else 1)
PY
if [ $DRY = 1 ]; then
    say "dry run: nothing written"
    [ -z "$(mount_point "$TARGET_ID")" ] || diskutil unmount "/dev/$TARGET_ID" >/dev/null 2>&1 || true
    exit 0
fi

# The ESP must be writable before anything is removed; a read-only or full FAT leaves the old kext in place.
touch "$TARGET_MP/.phx-vcn-write-test" 2>/dev/null || die "$TARGET_MP is not writable"
rm -f "$TARGET_MP/.phx-vcn-write-test"
FREE_KB=$(df -k "$TARGET_MP" | awk 'NR==2 {print $4}')
if [ -z "$FREE_KB" ] || [ "$FREE_KB" -lt 4096 ]; then die "less than 4 MiB free on $TARGET_MP"; fi
BK="$G/backup/vcn-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$BK"
ditto --norsrc --noextattr --noacl "$DEST" "$BK/PhxBringup.kext"
PART_UUID=$(diskutil info -plist "/dev/$TARGET_ID" | plutil -extract DiskUUID raw -o - -)
[ -n "$PART_UUID" ] || die "cannot identify the ESP partition UUID"
printf '%s\n%s\n%s\n' "$TARGET_ID" "$TARGET_MP" "$PART_UUID" > "$BK/esp"
printf '%s\n' "$BK" > "$G/backup/vcn-latest"
say "backup: $BK"

rm -rf "$DEST"
ditto --norsrc --noextattr --noacl "$SRC" "$DEST"
chmod -R 755 "$DEST"
cmp "$SRC/Contents/MacOS/PhxBringup" "$DEST/Contents/MacOS/PhxBringup" || die "copy differs from the candidate"
plutil -lint "$DEST/Contents/Info.plist" >/dev/null || die "copied Info.plist is invalid"
POST=$(kmutil print-diagnostics --bundle-path "$DEST" 2>&1 || true)
echo "$POST" | grep -q 'Dependencies: OK' || { echo "$POST"; die "copied kext fails dependency resolution"; }
sync
diskutil unmount "/dev/$TARGET_ID" >/dev/null 2>&1 || true

# Optional fallback: load VCN firmware in the boot script before AUTOLOAD_RLC (the order Linux uses). The inserted
# block is inert unless /Users/Shared/gpu/vcn-fw-at-boot exists, so the normal boot path is unchanged.
BRINGUP="$G/bringup5.sh"
if [ -f "$BRINGUP" ]; then
    if grep -q "vcn-fw-at-boot" "$BRINGUP"; then
        say "boot script already has the VCN preload block"
    else
        cp -p "$BRINGUP" "$BK/bringup5.sh"
        python3 - "$BRINGUP" <<'PY' || say "WARNING: could not add the VCN preload block to bringup5.sh"
import sys
p = sys.argv[1]
s = open(p).read()
marker = 'echo "== AUTOLOAD_RLC =="'
if marker not in s:
    raise SystemExit("AUTOLOAD_RLC marker not found; not touching " + p)
block = """# Optional VCN firmware preload, before AUTOLOAD_RLC (same order as Linux). Inert unless the marker file
# exists; only needed when loading VCN firmware later (selector 120) is refused by the PSP.
if [ -e /Users/Shared/gpu/vcn-fw-at-boot ]; then
  L ~/gpu/fw/vcn_4_0_0.bin 256 403600 13 0x900000
fi
"""
s = s.replace(marker, block + marker, 1)
open(p, "w").write(s)
print("patched", p)
PY
    fi
fi
say "installed PhxBringup (VCN selector 120) and $G/vcnctl"
echo
echo "next: reboot, then run:  sudo bash $HERE/vcn-test.sh"
echo "rollback:                sudo bash $HERE/rollback-vcn.sh"
echo "if the boot comes up without Metal 3 (phxboot-off appears), boot the firmware framebuffer and run:"
echo "                         sudo bash $HERE/rollback-vcn.sh --rearm   # restores $BK and re-arms phxboot, then reboot"
