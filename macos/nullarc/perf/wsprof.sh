#!/bin/bash
# usage: sudo ./wsprof.sh NAME — runs bench2.sh and samples WindowServer + Chrome GPU process for 8 s
T=$(cd "$(dirname "$0")"; pwd); N=${1:-ws}; U=${SUDO_USER:-$USER}
sudo -u $U bash -c "cd $T && ./bench2.sh $N >$T/$N.bench 2>&1" &
sleep 9
WS=$(pgrep -x WindowServer); GPU=$(pgrep -f "type=gpu-process.*ct-prof-$N" | head -1)
sample $WS 8 -file $T/$N.ws.sample >/dev/null 2>&1 &
[ -n "$GPU" ] && sample $GPU 8 -file $T/$N.gpu.sample >/dev/null 2>&1 &
wait; chown $U $T/$N.*; cat $T/$N.bench; ls -la $T/$N.*sample
