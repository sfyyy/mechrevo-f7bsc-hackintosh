#!/bin/bash
# f7bsc: display settings for nullarc PhxAccel at boot.
#   debug.phxfb_dbuf=1               double-buffered scan-out (no tearing; f7bsc patch)
#   debug.nvaccel_surface_gpu_on=0   WindowServer waits for its own composites (works around nullarc B-58 ghosting)
# The sysctls exist only once PhxAMDFB / the accelerator have started, so wait for them (up to 5 minutes).
LOG=/Users/Shared/f7bsc-logs/phxdisplay.log
mkdir -p /Users/Shared/f7bsc-logs
exec >>"$LOG" 2>&1
echo "$(date '+%F %T') start"
set_when_ready() {
  local name=$1 value=$2 i
  for i in $(seq 1 300); do
    if sysctl -n "$name" >/dev/null 2>&1; then
      sysctl -w "$name=$value" && echo "$(date '+%F %T') $name=$value after ${i}s"
      return 0
    fi
    sleep 1
  done
  echo "$(date '+%F %T') $name never appeared (PhxAccel not running this boot?)"
  return 1
}
set_when_ready debug.phxfb_dbuf 1 &
set_when_ready debug.nvaccel_surface_gpu_on 0 &
wait
sysctl debug.phxfb_dbuf_state debug.nvaccel_surface_gpu_on 2>/dev/null
echo "$(date '+%F %T') done"
