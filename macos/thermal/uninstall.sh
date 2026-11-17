#!/bin/bash
# Remove the clock cap LaunchDaemon. The cap itself lasts until the next reboot.
launchctl bootout system/local.gpuclk 2>/dev/null || true
rm -f /Library/LaunchDaemons/local.gpuclk.plist /usr/local/libexec/gpuclk.sh /usr/local/libexec/gpucg.sh
echo "removed; reboot to return to nullarc's fixed 2600 MHz"
