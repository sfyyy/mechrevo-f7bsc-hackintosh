#!/bin/bash
# sudo bash install.sh           install and apply now
# sudo bash install.sh --remove  remove (settings go back to driver defaults after reboot)
#
# Plugin knobs added to nvrm610.conf (read by every process when it loads the Metal plugin):
#   NVMTL_WS_WAIT_COMPLETION=1             WindowServer waits for each of its own composites (with surface_gpu_on=0)
#   NVMTL_SCHEDULE_AFTER_SURFACE_WRITES=1  a command buffer that writes a shared IOSurface reports "scheduled" only after
#                                          the GPU has finished it, so CoreAnimation in other processes (loginwindow's
#                                          lock screen) hands WindowServer finished surfaces
set -u
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
P=/Library/LaunchDaemons/local.f7bsc.phxdisplay.plist
S=/usr/local/libexec/f7bsc-phxdisplay.sh
CONF=/Library/GPUBundles/nvmtl/nvrm610.conf
KNOBS="NVMTL_WS_WAIT_COMPLETION=1 NVMTL_SCHEDULE_AFTER_SURFACE_WRITES=1"
if [ "${1:-}" = "--remove" ]; then
  launchctl bootout system "$P" 2>/dev/null; rm -f "$P" "$S"
  if [ -f "$CONF" ]; then for k in $KNOBS; do sed -i '' "/^$k\$/d" "$CONF"; done; fi
  echo "removed"; exit 0
fi
mkdir -p /usr/local/libexec
install -m 755 -o root -g wheel "$HERE/phxdisplay.sh" "$S"
install -m 644 -o root -g wheel "$HERE/local.f7bsc.phxdisplay.plist" "$P"
if [ -f "$CONF" ]; then
  [ -e "$CONF.bak-f7bsc" ] || cp -p "$CONF" "$CONF.bak-f7bsc"
  for k in $KNOBS; do
    grep -q "^$k\$" "$CONF" || { echo "$k" >> "$CONF" && echo "added $k to $CONF"; }
  done
else
  echo "warning: $CONF not found (nullarc not installed?) - plugin knobs not set"
fi
launchctl bootout system "$P" 2>/dev/null
launchctl bootstrap system "$P" && echo "LaunchDaemon loaded"
sleep 2; tail -5 /Users/Shared/f7bsc-logs/phxdisplay.log
echo "plugin knobs take effect for each process when it next starts: log out or reboot"
