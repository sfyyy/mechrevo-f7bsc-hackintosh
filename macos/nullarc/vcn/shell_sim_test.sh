#!/bin/bash
# Offline simulation of install-vcn.sh / rollback-vcn.sh / vcn-test.sh on a fake ESP and a fake vcnctl.
# It never touches a real disk: diskutil, ioreg and shutdown are shims in a private PATH, and the
# scripts run with their root check removed against a scratch directory.
#   WORK=~/Documents/apple/work macos/nullarc/vcn/shell_sim_test.sh
set -euo pipefail
WORK=${WORK:-$HOME/Documents/apple/work}
HERE=$(cd "$(dirname "$0")" && pwd)
BRINGUP_SRC=${BRINGUP_SRC:-$WORK/nullarc-0.4.0-alpha.orig/tools/phxctl/bringup5.sh}
for f in install-vcn.sh rollback-vcn.sh vcn-test.sh PhxBringup-vcn.kext vcn-ref/vcn_4_0_0.bin \
         nullarc-0.4.0-alpha/tools/phxctl/vcnctl.c mechrevo-f7bsc-hackintosh/EFI/OC/config.plist; do
    [ -e "$WORK/$f" ] || { echo "missing $WORK/$f"; exit 1; }
done
SIM=$(mktemp -d /tmp/phx-vcn-sim.XXXXXX)
trap 'rm -rf "$SIM"' EXIT
export SIMDIR="$SIM"
fail() { echo "FAIL: $*"; exit 1; }

# ---------- shims ----------
mkdir -p "$SIM/bin" "$SIM/state"
cat > "$SIM/bin/diskutil" <<'EOF'
#!/bin/bash
case "$1/$2" in
  list/-plist) cat "$SIMDIR/state/list.plist" ;;
  info/-plist) if [ -e "$SIMDIR/state/mounted" ]; then cat "$SIMDIR/state/info_mounted.plist"; else cat "$SIMDIR/state/info_unmounted.plist"; fi ;;
  mount/*) touch "$SIMDIR/state/mounted"; echo mounted ;;
  unmount/*) rm -f "$SIMDIR/state/mounted"; echo unmounted ;;
  *) exit 1 ;;
esac
EOF
cat > "$SIM/bin/ioreg" <<'EOF'
#!/bin/bash
printf '    | | |   "phx-build" = %s\n' "${SIM_BUILD:-38}"
EOF
printf '#!/bin/bash\nexit 0\n' > "$SIM/bin/shutdown"
chmod +x "$SIM/bin/"*

plist() { printf '%s\n' '<?xml version="1.0" encoding="UTF-8"?>' '<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">' "$*"; }
plist '<plist version="1.0"><dict><key>AllDisksAndPartitions</key><array><dict><key>Partitions</key><array><dict><key>Content</key><string>EFI</string><key>DeviceIdentifier</key><string>disk9s1</string></dict></array></dict></array></dict></plist>' > "$SIM/state/list.plist"
plist '<plist version="1.0"><dict><key>MountPoint</key><string>'"$SIM"'/esp</string></dict></plist>' > "$SIM/state/info_mounted.plist"
plist '<plist version="1.0"><dict><key>MountPoint</key><string></string></dict></plist>' > "$SIM/state/info_unmounted.plist"

# rewrite a script: no root check, private PATH, scratch ESP/G, HERE where the installer expects it
rewrite() {
    python3 - "$1" "$2" "$3" "$4" <<'PY'
import sys
src, here, g, out = sys.argv[1:5]
lines = open(src).read().split('\n')
lines = [l for l in lines if l != '[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }']
lines.insert(1, 'export PATH="%s/bin:$PATH"' % here)
s = '\n'.join(lines)
s = s.replace('HERE=$(cd "$(dirname "$0")" && pwd)', 'HERE=' + here)
s = s.replace('G=/Users/Shared/gpu', 'G=' + g)
open(out, 'w').write(s)
PY
}

# ---------- install / rollback ----------
mkdir -p "$SIM/fakegpu/fw" "$SIM/esp/EFI/OC/Kexts" "$SIM/nullarc-0.4.0-alpha/tools/phxctl"
cp -R "$WORK/PhxBringup-vcn.kext" "$SIM/"
cp -R "$WORK/vcn-ref" "$SIM/"
cp "$WORK/nullarc-0.4.0-alpha/tools/phxctl/vcnctl.c" "$SIM/nullarc-0.4.0-alpha/tools/phxctl/"
cp -R "$WORK/PhxBringup-vcn.kext" "$SIM/esp/EFI/OC/Kexts/PhxBringup.kext"
echo old > "$SIM/esp/EFI/OC/Kexts/PhxBringup.kext/Contents/OLD_MARKER"
cp "$WORK/mechrevo-f7bsc-hackintosh/EFI/OC/config.plist" "$SIM/esp/EFI/OC/config.plist"
if [ -f "$BRINGUP_SRC" ]; then
    if grep -q vcn-fw-at-boot "$BRINGUP_SRC"; then fail "BRINGUP_SRC must be an unpatched boot script"; fi
    cp "$BRINGUP_SRC" "$SIM/fakegpu/bringup5.sh"
else
    printf '%s\n' 'echo "== AUTOLOAD_RLC =="' > "$SIM/fakegpu/bringup5.sh"
fi
rewrite "$WORK/install-vcn.sh" "$SIM" "$SIM/fakegpu" "$SIM/install.sh"
rewrite "$WORK/rollback-vcn.sh" "$SIM" "$SIM/fakegpu" "$SIM/rollback.sh"
chmod +x "$SIM/install.sh" "$SIM/rollback.sh"
bash -n "$SIM/install.sh" && bash -n "$SIM/rollback.sh"

bash "$SIM/install.sh" --tool-only >/dev/null || fail "tool-only install"
[ -x "$SIM/fakegpu/vcnctl" ] || fail "tool-only install did not build vcnctl"
"$SIM/fakegpu/vcnctl" check-firmware "$WORK/vcn-ref/vcn_4_0_0.bin" >/dev/null || fail "real firmware parser"
[ ! -e "$SIM/state/mounted" ] || fail "tool-only install mounted ESP"
[ ! -e "$SIM/fakegpu/backup" ] || fail "tool-only install created an ESP backup"
[ -f "$SIM/esp/EFI/OC/Kexts/PhxBringup.kext/Contents/OLD_MARKER" ] || fail "tool-only install changed kext"
printf 'too short' > "$SIM/truncated-fw.bin"
if "$SIM/fakegpu/vcnctl" check-firmware "$SIM/truncated-fw.bin" >/dev/null 2>&1; then
    fail "firmware parser accepted a truncated header"
fi
clang -O2 -Wall -Wextra -Werror "$SIM/nullarc-0.4.0-alpha/tools/phxctl/vcnctl.c" "$HERE/vcnctl_mock.c" \
    -framework IOKit -framework CoreFoundation -o "$SIM/vcnctl-mock"
"$SIM/vcnctl-mock" status >/dev/null || fail "vcnctl HELLO/status"
VCN_TEST_FIRMWARE="$WORK/vcn-ref/vcn_4_0_0.bin" "$SIM/vcnctl-mock" init "$WORK/vcn-ref/vcn_4_0_0.bin" \
    >"$SIM/mock-init.log" || fail "vcnctl HELLO/stage/init"
grep -q '^init: code 0 ' "$SIM/mock-init.log" || fail "vcnctl did not complete init"

bash "$SIM/install.sh" --dry-run >/dev/null || fail "install --dry-run"
bash "$SIM/install.sh" >/dev/null || fail "install"
cmp -s "$SIM/PhxBringup-vcn.kext/Contents/MacOS/PhxBringup" "$SIM/esp/EFI/OC/Kexts/PhxBringup.kext/Contents/MacOS/PhxBringup" || fail "installed binary differs"
[ -f "$SIM/esp/EFI/OC/Kexts/PhxBringup.kext/Contents/OLD_MARKER" ] && fail "old kext not replaced"
BK=$(cat "$SIM/fakegpu/backup/vcn-latest")
[ -f "$BK/PhxBringup.kext/Contents/OLD_MARKER" ] || fail "backup lost the old kext"
[ -f "$BK/bringup5.sh" ] || fail "boot script was not backed up"
[ ! -e "$SIM/state/mounted" ] || fail "ESP left mounted"
[ "$(grep -c vcn-fw-at-boot "$SIM/fakegpu/bringup5.sh")" = 1 ] || fail "boot block not inserted exactly once"
bash "$SIM/rollback.sh" >/dev/null || fail "rollback"
[ -f "$SIM/esp/EFI/OC/Kexts/PhxBringup.kext/Contents/OLD_MARKER" ] || fail "rollback did not restore the old kext"
[ ! -e "$SIM/state/mounted" ] || fail "ESP left mounted after rollback"
bash "$SIM/install.sh" >/dev/null || fail "second install"
[ "$(grep -c vcn-fw-at-boot "$SIM/fakegpu/bringup5.sh")" = 1 ] || fail "boot block duplicated"
bash "$SIM/rollback.sh" >/dev/null || fail "second rollback"
[ -f "$SIM/esp/EFI/OC/Kexts/PhxBringup.kext/Contents/OLD_MARKER" ] || fail "second rollback did not restore the old kext"
echo "install/rollback simulation: OK"

# ---------- vcn-test paths ----------
cp "$WORK/vcn-ref/vcn_4_0_0.bin" "$SIM/fakegpu/fw/"
cat > "$SIM/fakegpu/vcnctl" <<'EOF'
#!/bin/bash
echo "CALL $*" >> "$SIMDIR/calls.log"
case "$1" in
  check-firmware) [ "${SIM_FAIL_OP:-}" != check-firmware ] ;;
  status) [ "${SIM_FAIL_OP:-}" != status ] || exit 1; echo "VCN: down" ;;
  init) [ "$2" = "--tmr" ] && echo "init tmr=$3" || echo "init file=$2" ;;
  ring|fence) [ "$1" != "${SIM_FAIL_OP:-}" ] || exit 1; echo "$1 ok" ;;
  down) echo "down: code 0" ;;
esac
EOF
chmod +x "$SIM/fakegpu/vcnctl"
rewrite "$WORK/vcn-test.sh" "$SIM" "$SIM/fakegpu" "$SIM/vcn-test.sh"
chmod +x "$SIM/vcn-test.sh"
bash -n "$SIM/vcn-test.sh"
rm -f "$SIM/calls.log"; bash "$SIM/vcn-test.sh" >/dev/null || fail "vcn-test normal"
grep -q '^CALL init /' "$SIM/calls.log" || fail "normal path did not call init"
grep -q '^CALL fence 6' "$SIM/calls.log" || fail "fence not called"
grep -q '^CALL down' "$SIM/calls.log" || fail "power down not called"
rm -f "$SIM/calls.log"; touch "$SIM/fakegpu/vcn-fw-at-boot"
{ echo 'loadfw x fwtype 13 @fwbuf+0x900000 -> kr 0x00000000  status 0x00000000  10 us  fw_addr 0x0000008008C00000'
  echo 'loadfw x fwtype 13 @fwbuf+0x900000 -> kr 0x00000000  status 0x00000001  10 us  fw_addr 0x0000008008E00000'
} > "$SIM/fakegpu/fwaddr.log"
bash "$SIM/vcn-test.sh" >/dev/null || fail "vcn-test preload"
grep -q '^CALL init --tmr 0x0000008008C00000' "$SIM/calls.log" || fail "preload TMR not used"
rm -f "$SIM/calls.log"; bash "$SIM/vcn-test.sh" --keep >/dev/null || fail "vcn-test --keep"
grep -q '^CALL down' "$SIM/calls.log" && fail "--keep still powered down"
rm -f "$SIM/calls.log"; bash "$SIM/vcn-test.sh" --tmr 0x0000008008D00000 >/dev/null || fail "--tmr override"
grep -q '^CALL init --tmr 0x0000008008D00000' "$SIM/calls.log" || fail "--tmr override ignored"
rm -f "$SIM/calls.log"
if SIM_FAIL_OP=ring bash "$SIM/vcn-test.sh" --keep >/dev/null; then fail "ring failure reported success"; fi
if grep -q '^CALL fence' "$SIM/calls.log"; then fail "fence ran after ring failure"; fi
grep -q '^CALL down' "$SIM/calls.log" || fail "ring failure did not clean up"
rm -f "$SIM/calls.log"
if SIM_BUILD=37 bash "$SIM/vcn-test.sh" >/dev/null; then fail "old build reported success"; fi
[ ! -e "$SIM/calls.log" ] || fail "called the old build"
for failed_op in check-firmware status; do
    rm -f "$SIM/calls.log"
    if SIM_FAIL_OP="$failed_op" bash "$SIM/vcn-test.sh" >/dev/null; then fail "$failed_op reported success"; fi
    if grep -q '^CALL init' "$SIM/calls.log"; then fail "init ran after $failed_op failed"; fi
done
echo "vcn-test simulation: OK"
echo "ALL SHELL SIMULATIONS PASSED"
