#!/bin/bash
# sudo bash wslive.sh — sample WindowServer for 8 s as things are now (e.g. during a Tencent Meeting share), 60 Hz anim on top
D=$(cd "$(dirname "$0")" && pwd); U=${SUDO_USER:-$USER}
sudo -u "$U" "$D/anim" 12 >/dev/null 2>&1 &
sleep 2
a=$(sysctl -n debug.phxfb_dbuf_flips)
sample "$(pgrep -x WindowServer)" 8 -mayDie -file /tmp/ws-live.txt >/dev/null 2>&1
b=$(sysctl -n debug.phxfb_dbuf_flips); echo "display $(( (b-a)/8 )) flips/s"
chmod 644 /tmp/ws-live.txt; wait; echo done: /tmp/ws-live.txt
