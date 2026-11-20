#!/bin/zsh
# ab.sh [reps] — 60 Hz anim + SCK capture 25/60 fps; prints capture fps and display flips/s, plus a no-capture baseline
D=${0:A:h}; N=${1:-2}; fl(){ sysctl -n debug.phxfb_dbuf_flips; }
echo "surface_gpu_on=$(sysctl -n debug.nvaccel_surface_gpu_on) load=$(sysctl -n vm.loadavg)"
$D/anim 300 >/dev/null 2>&1 & AP=$!; sleep 2
a=$(fl); sleep 5; b=$(fl); echo "no capture: $(( (b-a)/5 )) flips/s"
for rep in $(seq 1 $N); do for r in 25 60; do
  a=$(fl); $D/sck 2560 1440 $r BGRA 6 > /tmp/ab-sck.out 2>&1; b=$(fl)
  echo "req$r: $(grep fps /tmp/ab-sck.out | sed 's/.*frames, //;s/, lock.*//')  display $(( (b-a)/6 )) flips/s"
done; done
kill $AP 2>/dev/null
