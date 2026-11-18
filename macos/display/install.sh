#!/bin/bash
# sudo bash install.sh           install and apply now
# sudo bash install.sh --remove  remove (settings go back to driver defaults after reboot)
set -u
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
HERE=$(cd "$(dirname "$0")" && pwd)
P=/Library/LaunchDaemons/local.f7bsc.phxdisplay.plist
S=/usr/local/libexec/f7bsc-phxdisplay.sh
CONF=/Library/GPUBundles/nvmtl/nvrm610.conf
if [ "${1:-}" = "--remove" ]; then
  launchctl bootout system "$P" 2>/dev/null; rm -f "$P" "$S"
  [ -f "$CONF" ] && sed -i '' '/^NVMTL_WS_WAIT_COMPLETION=1$/d' "$CONF"
  echo "removed"; exit 0
fi
mkdir -p /usr/local/libexec
install -m 755 -o root -g wheel "$HERE/phxdisplay.sh" "$S"
install -m 644 -o root -g wheel "$HERE/local.f7bsc.phxdisplay.plist" "$P"
if [ -f "$CONF" ] && ! grep -q '^NVMTL_WS_WAIT_COMPLETION=1$' "$CONF"; then
  cp -p "$CONF" "$CONF.bak-f7bsc" && echo 'NVMTL_WS_WAIT_COMPLETION=1' >> "$CONF" && echo "added NVMTL_WS_WAIT_COMPLETION=1 to $CONF"
fi
launchctl bootout system "$P" 2>/dev/null
launchctl bootstrap system "$P" && echo "LaunchDaemon loaded"
sleep 2; tail -5 /Users/Shared/f7bsc-logs/phxdisplay.log
