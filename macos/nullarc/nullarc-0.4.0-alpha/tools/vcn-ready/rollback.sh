#!/bin/sh
# Remove only the watcher. Leave VCN and live codec sessions in their current state.
set -eu
[ "$(id -u)" = 0 ] || { echo "Run as root"; exit 1; }
base=/Library/PrivilegedHelperTools/org.nullarc.vcn-ready
if /bin/launchctl print system/org.nullarc.vcn-ready >/dev/null 2>&1; then
    /bin/launchctl bootout system/org.nullarc.vcn-ready
fi
/bin/rm -f /Library/LaunchDaemons/org.nullarc.vcn-ready.plist
/bin/rm -f "$base/watch" "$base/ensure.sh" "$base/registry.py" "$base/vcn_4_0_0.bin"
if [ -d "$base" ]; then /bin/rmdir "$base"; fi
