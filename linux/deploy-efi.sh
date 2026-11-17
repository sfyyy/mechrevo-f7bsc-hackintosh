#!/bin/bash
# Deploy this repo's EFI/OC to an ESP with the machine's private SMBIOS identity filled in. Run with sudo.
#
#   sudo linux/deploy-efi.sh                 Arch ESP (/boot): replaces EFI/OC only, EFI/BOOT stays systemd-boot,
#                                            and installs the systemd-boot "macOS" entry if it is missing
#   sudo linux/deploy-efi.sh /mnt/usbefi --with-boot
#                                            another ESP (e.g. the USB stick): also installs OpenCore's EFI/BOOT
#
# The current EFI/OC is archived to backups/ (git-ignored) first. EFI/OC on the target becomes an exact copy of the
# repo (old config variants and *.bak files are removed).
set -euo pipefail
REPO=$(cd "$(dirname "$0")/.." && pwd)
ESP=/boot
WITH_BOOT=0
for a in "$@"; do
  case "$a" in
    --with-boot) WITH_BOOT=1 ;;
    *) ESP=$a ;;
  esac
done
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
mountpoint -q "$ESP" || { echo "$ESP is not a mount point"; exit 1; }
[ -f "$REPO/smbios.private.json" ] || { echo "missing $REPO/smbios.private.json (see README)"; exit 1; }
if [ $WITH_BOOT = 1 ] && [ -f "$ESP/loader/loader.conf" ]; then
  echo "$ESP boots systemd-boot from EFI/BOOT: refusing --with-boot"; exit 1
fi

python3 - "$REPO/EFI/OC" <<'EOF'
import os, plistlib, sys
oc = sys.argv[1]
c = plistlib.load(open(f'{oc}/config.plist', 'rb'))
need = [f'ACPI/{a["Path"]}' for a in c['ACPI']['Add'] if a['Enabled']]
need += [f'Drivers/{d["Path"]}' for d in c['UEFI']['Drivers'] if d['Enabled']]
for k in c['Kernel']['Add']:
    if k['Enabled']:
        need.append(f'Kexts/{k["BundlePath"]}/{k["PlistPath"]}')
        if k['ExecutablePath']:
            need.append(f'Kexts/{k["BundlePath"]}/{k["ExecutablePath"]}')
missing = [p for p in need if not os.path.exists(f'{oc}/{p}')]
if missing:
    sys.exit('missing in EFI/OC (see README, "需要自行下载"):\n  ' + '\n  '.join(missing))
EOF

STAGE=$(mktemp -d)
trap 'rm -rf "$STAGE"' EXIT
cp -r "$REPO/EFI/OC" "$STAGE/OC"
python3 - "$REPO/smbios.private.json" "$STAGE/OC/config.plist" <<'EOF'
import json, plistlib, sys
ids = json.load(open(sys.argv[1]))
c = plistlib.load(open(sys.argv[2], 'rb'))
g = c['PlatformInfo']['Generic']
for k in ('SystemProductName', 'SystemSerialNumber', 'MLB', 'SystemUUID'):
    g[k] = ids[k]
g['ROM'] = bytes.fromhex(ids['ROM'].replace(':', ''))
assert len(g['ROM']) == 6
with open(sys.argv[2], 'wb') as f:
    plistlib.dump(c, f, sort_keys=False)
EOF

if [ -d "$ESP/EFI/OC" ]; then
  TS=$(date +%Y%m%d-%H%M%S)
  BK="$REPO/backups/OC-$(basename "$ESP")-$TS.tar.gz"
  mkdir -p "$REPO/backups"
  tar czf "$BK" -C "$ESP/EFI" OC
  [ -n "${SUDO_USER:-}" ] && chown -R "$SUDO_USER": "$REPO/backups"
  echo "backup: $BK"
fi

mkdir -p "$ESP/EFI/OC"
rsync -rc --delete --itemize-changes "$STAGE/OC/" "$ESP/EFI/OC/"   # vfat: content only, no owners/modes/times
if [ $WITH_BOOT = 1 ]; then
  mkdir -p "$ESP/EFI/BOOT"
  cp "$REPO/EFI/BOOT/BOOTx64.efi" "$ESP/EFI/BOOT/BOOTx64.efi"
fi
if [ -d "$ESP/loader/entries" ] && [ ! -f "$ESP/loader/entries/macos.conf" ]; then
  cp "$REPO/linux/macos.conf" "$ESP/loader/entries/macos.conf"
  echo "installed systemd-boot entry: $ESP/loader/entries/macos.conf"
fi
sync
cmp "$STAGE/OC/config.plist" "$ESP/EFI/OC/config.plist"
echo "deployed to $ESP/EFI/OC"
