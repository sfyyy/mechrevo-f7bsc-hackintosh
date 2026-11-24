#!/bin/bash
# Restore the PhxBringup.kext that install-vcn.sh replaced (the latest backup), and unmount the ESP.
#   sudo bash rollback-vcn.sh
set -euo pipefail
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
G=/Users/Shared/gpu
MARK="$G/backup/vcn-latest"
[ -f "$MARK" ] || { echo "no backup recorded in $MARK"; exit 1; }
BK=$(cat "$MARK")
[ -d "$BK/PhxBringup.kext" ] || { echo "backup $BK is incomplete"; exit 1; }
ID=$(sed -n '1p' "$BK/esp")
[ -n "${ID:-}" ] || { echo "backup has no ESP record"; exit 1; }
PART_UUID=$(sed -n '3p' "$BK/esp")
if [ -n "$PART_UUID" ]; then
    ID=$(diskutil info -plist "$PART_UUID" | plutil -extract DeviceIdentifier raw -o - -) || {
        echo "cannot locate the backed-up ESP with partition UUID $PART_UUID"; exit 1;
    }
fi
mount_point() {
    python3 - "$1" <<'PY'
import plistlib, subprocess, sys
d = plistlib.loads(subprocess.check_output(["diskutil", "info", "-plist", "/dev/" + sys.argv[1]]))
print(d.get("MountPoint") or "")
PY
}
mp=$(mount_point "$ID" || true)
if [ -z "$mp" ]; then
    diskutil mount "/dev/$ID" >/dev/null || { echo "cannot mount /dev/$ID"; exit 1; }
    mp=$(mount_point "$ID" || true)
fi
[ -n "$mp" ] || { echo "cannot mount /dev/$ID"; exit 1; }
DEST="$mp/EFI/OC/Kexts/PhxBringup.kext"
[ -d "$DEST" ] || { echo "$DEST is gone; refusing to guess"; exit 1; }
CUR="$G/backup/vcn-current-$(date +%Y%m%d-%H%M%S)"
mkdir -p "$CUR"
ditto --norsrc --noextattr --noacl "$DEST" "$CUR/PhxBringup.kext"
rm -rf "$DEST"
ditto --norsrc --noextattr --noacl "$BK/PhxBringup.kext" "$DEST"
cmp "$BK/PhxBringup.kext/Contents/MacOS/PhxBringup" "$DEST/Contents/MacOS/PhxBringup" || { echo "restore failed"; exit 1; }
sync
diskutil unmount "/dev/$ID" >/dev/null 2>&1 || true
if [ -f "$BK/bringup5.sh" ]; then
    BRINGUP="$G/bringup5.sh"
    [ -f "$BRINGUP" ] || { echo "$BRINGUP is missing; refusing to restore an unrelated boot script"; exit 1; }
    cp -p "$BRINGUP" "$CUR/bringup5.sh"
    cp -p "$BK/bringup5.sh" "$BRINGUP"
    cmp -s "$BK/bringup5.sh" "$BRINGUP" || { echo "bringup5.sh restore failed"; exit 1; }
    echo "restored $BRINGUP"
fi
echo "restored $BK/PhxBringup.kext to $DEST (previous current kept in $CUR)"
echo "reboot to load it"
