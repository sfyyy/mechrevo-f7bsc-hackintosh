#!/bin/bash
# usage: [M=all|empty|scroll|canvas|box] bench3.sh NAME [secs] — bench2 on page2.html (load selected by M), plus a native
# CVDisplayLink probe (vblprobe) running at the same time: if the display link itself skips, the stall is below Chrome.
T=$(cd "$(dirname "$0")"; pwd)
[ -x "$T/vblprobe" ] || xcrun clang -fobjc-arc -O2 -Wno-deprecated-declarations -o "$T/vblprobe" "$T/vblprobe.m" -framework Foundation -framework CoreVideo -framework CoreGraphics -F/System/Library/PrivateFrameworks -framework SkyLight
N=$1; S=${2:-25}; D=/tmp/ct-prof-$N; rm -rf $D; LOG=$T/$N.log
"$T/vblprobe" $((S+4)) > $T/$N.vbl 2>&1 &
VP=$!
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --user-data-dir=$D --no-first-run --no-default-browser-check --enable-logging=stderr --v=0 --window-size=${W:-1600,1000} --window-position=0,0 "file://$T/page2.html?m=${M:-all}" >$LOG 2>&1 &
PID=$!; sleep 5
top -l $((S/2)) -s 2 -stats pid,cpu,command | awk '$3=="WindowServer"{n++; if(n>1){s+=$2;c++}} END{printf "WS %.1f%% ", s/c}'
grep -o 'FPS [0-9.]* worst_ms [0-9]* jank [0-9]* hist [0-9/]* worstjs [0-9.]* longjs [0-9]*' $LOG | tail -n +2 | awk '{f+=$2; j+=$6; if($4>w)w=$4; n++; split($8,h,"/"); for(i=1;i<=5;i++)H[i]+=h[i]; if($10>wj)wj=$10; lj+=$12} END{printf "FPS %.1f jank %d worst %d (%d windows) hist %d/%d/%d/%d/%d worstjs %.1f longjs %d ", f/n, j, w, n, H[1],H[2],H[3],H[4],H[5], wj, lj}'
kill $PID; sleep 2; pkill -f "user-data-dir=$D"; wait $VP 2>/dev/null; tail -1 $T/$N.vbl
