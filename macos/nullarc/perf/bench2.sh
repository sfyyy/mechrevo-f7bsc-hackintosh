#!/bin/bash
# usage: [ENV=..] bench2.sh NAME [secs] — runs page.html in a temp Chrome profile; mean page FPS, total jank, mean WindowServer CPU
T=$(cd "$(dirname "$0")"; pwd); N=$1; S=${2:-25}; D=/tmp/ct-prof-$N; rm -rf $D; LOG=$T/$N.log
"/Applications/Google Chrome.app/Contents/MacOS/Google Chrome" --user-data-dir=$D --no-first-run --no-default-browser-check --enable-logging=stderr --v=0 --window-size=${W:-1600,1000} --window-position=0,0 file://$T/page.html >$LOG 2>&1 &
PID=$!; sleep 5
top -l $((S/2)) -s 2 -stats pid,cpu,command | awk '$3=="WindowServer"{n++; if(n>1){s+=$2;c++}} END{printf "WS %.1f%% ", s/c}'
grep -o 'FPS [0-9.]* worst_ms [0-9]* jank [0-9]*' $LOG | tail -n +2 | awk '{f+=$2; j+=$6; if($4>w)w=$4; n++} END{printf "FPS %.1f jank %d worst %d (%d windows)\n", f/n, j, w, n}'
kill $PID; sleep 2; pkill -f "user-data-dir=$D"
