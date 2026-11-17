#!/bin/bash
# Install the 780M clock cap (800 MHz) + clock gating as a LaunchDaemon. Run with sudo from this directory.
# gpuclk.sh / gpucg.sh / local.gpuclk.plist come from QiRanChenk/MECHREVO-WUJIE15XA-8745HS-Hackintosh tools/ (MECHREVO WUJIE15XA, same Phoenix SMU 13.0.4).
set -e
cd "$(dirname "$0")"
[ -x /Users/Shared/gpu/phxctl/phxctl ] || { echo "missing /Users/Shared/gpu/phxctl/phxctl (nullarc not installed?)"; exit 1; }
sysctl -n kern.bootargs | grep -qw phxdebug=1 || { echo "boot-arg phxdebug=1 missing: phxctl cannot talk to the SMU"; exit 1; }
mkdir -p /usr/local/libexec
install -m 755 -o root -g wheel gpuclk.sh /usr/local/libexec/gpuclk.sh
install -m 755 -o root -g wheel gpucg.sh /usr/local/libexec/gpucg.sh
install -m 644 -o root -g wheel local.gpuclk.plist /Library/LaunchDaemons/local.gpuclk.plist
launchctl bootout system/local.gpuclk 2>/dev/null || true
launchctl bootstrap system /Library/LaunchDaemons/local.gpuclk.plist
echo "installed; runs ~15 s after NMXAccel is up at every boot; log: /var/log/gpuclk.log"
