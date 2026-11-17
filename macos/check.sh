#!/bin/bash
# Read-only status for the thermal fix and the HiDPI question. Run with sudo; paste the output back.
P=/Users/Shared/gpu/phxctl/phxctl
echo "== $(date '+%F %T')  $(sw_vers -productVersion) ($(sw_vers -buildVersion))"
echo "== boot-args: $(sysctl -n kern.bootargs)"
echo "== CPU power management"
kextstat 2>/dev/null | grep -iE 'ryzen|smcamd|PhxBringup|PhxAccel' | awk '{print $6, $7}'
sysctl debug.ryzenidle debug.ryzenidle_port 2>/dev/null || echo "debug.ryzenidle: absent (AMDRyzenCPUPowerManagement not loaded)"
echo "== GPU"
if [ -x $P ]; then
  $P regs 0x2004 | sed 's/$/   (GRBM_STATUS: 0xa800302c = CP stuck busy, 0x0000302c = idle)/'
  $P smuinfo 2>&1 | tail -n 15
else
  echo "no $P"
fi
[ -f /var/log/gpuclk.log ] && { echo "-- /var/log/gpuclk.log (tail)"; tail -n 8 /var/log/gpuclk.log; }
[ -x /usr/local/libexec/gpucg.sh ] && { echo "-- gpucg status"; /usr/local/libexec/gpucg.sh status; }
echo "== load"
uptime
ps -Aceo pcpu,comm -r | head -n 8
echo "== display (HiDPI)"
ioreg -lw0 -r -c PhxAMDFB 2>/dev/null | grep -E '"PhxFB(Width|Height|RefreshMilliHz|HiResModeFlag)"|"IOFBScalerInfo"' | sed 's/^[ |]*//'
ioreg -lw0 -r -c AppleDisplay 2>/dev/null | grep -E '"Display(VendorID|ProductID)"' | sed 's/^[ |]*//'
ls /Library/Displays/Contents/Resources/Overrides 2>/dev/null
system_profiler SPDisplaysDataType 2>/dev/null | grep -E 'Chipset|Metal|Resolution|UI Looks like|Refresh'
echo "== recent WindowServer crashes"
ls -t /Library/Logs/DiagnosticReports 2>/dev/null | grep -i windowserver | head -n 5
