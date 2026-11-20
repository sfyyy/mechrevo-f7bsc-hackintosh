#!/bin/bash
# run: sudo bash wssample.sh   — samples WindowServer with/without an SCK capture (60 Hz animation running in both)
D=$(cd "$(dirname "$0")" && pwd); U=${SUDO_USER:-$USER}
WS=$(pgrep -x WindowServer)
fl(){ sysctl -n debug.phxfb_dbuf_flips; }
sudo -u "$U" "$D/anim" 30 >/dev/null 2>&1 &
sleep 2
a=$(fl); sample "$WS" 8 -mayDie -file /tmp/ws-nocap.txt >/dev/null 2>&1; b=$(fl); echo "no capture: $(( (b-a)/8 )) flips/s"
sudo -u "$U" "$D/sck" 2560 1440 25 BGRA 13 > /tmp/ws-sck.out 2>&1 &
sleep 3
a=$(fl); sample "$WS" 8 -mayDie -file /tmp/ws-cap.txt >/dev/null 2>&1; b=$(fl); echo "capture:    $(( (b-a)/8 )) flips/s"
wait
chmod 644 /tmp/ws-nocap.txt /tmp/ws-cap.txt
grep -v radv /tmp/ws-sck.out | head -1
echo done: /tmp/ws-nocap.txt /tmp/ws-cap.txt
