#!/bin/sh
# Install the boot/wake watcher on the Phoenix host; root owns all installed files.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
base=/Library/PrivilegedHelperTools/org.nullarc.vcn-ready
plist=/Library/LaunchDaemons/org.nullarc.vcn-ready.plist
firmware=/Users/Shared/gpu/fw/vcn_4_0_0.bin
[ "$(id -u)" = 0 ] || { echo "Run as root"; exit 1; }
if [ -e "$base" ] || [ -e "$plist" ]; then
    echo "Existing org.nullarc.vcn-ready installation; remove it before installing again" >&2
    exit 1
fi
[ -x /Users/Shared/gpu/vcnctl ]
work=$(mktemp -d /private/var/tmp/phx-vcn-ready.XXXXXXXX)
trap 'rm -rf "$work"' EXIT
/usr/bin/clang -std=c11 -arch x86_64 -fblocks -Wall -Wextra -Werror -O2 \
    -framework IOKit -framework CoreFoundation "$here/watch.c" -o "$work/watch"
/usr/bin/codesign --force --sign - "$work/watch"
/usr/bin/codesign --verify --strict "$work/watch"
/usr/bin/plutil -lint "$here/org.nullarc.vcn-ready.plist"
/bin/sh -n "$here/ensure.sh"
/usr/bin/install -m 644 "$firmware" "$work/vcn_4_0_0.bin"
hash=$(/usr/bin/shasum -a 256 "$work/vcn_4_0_0.bin")
[ "${hash%% *}" = 5633a5212d41e1f3932c1e7e630b1f2c8c4d27bc53ce8149c3be75d1d832053c ]
/Users/Shared/gpu/vcnctl check-firmware "$work/vcn_4_0_0.bin"
if [ ! -d /Library/PrivilegedHelperTools ]; then
    /usr/bin/install -d -o root -g wheel -m 755 /Library/PrivilegedHelperTools
fi
/bin/mkdir "$base"
# This invocation created the directory. Clean up its files if bootstrap fails.
trap 'rc=$?; rm -rf "$work"; if [ "$rc" -ne 0 ]; then rm -f "$plist"; rm -rf "$base"; fi' EXIT
/usr/bin/install -o root -g wheel -m 755 "$work/watch" "$base/watch"
/usr/bin/install -o root -g wheel -m 644 "$here/ensure.sh" "$base/ensure.sh"
/usr/bin/install -o root -g wheel -m 644 "$here/registry.py" "$base/registry.py"
/usr/bin/install -o root -g wheel -m 644 "$work/vcn_4_0_0.bin" "$base/vcn_4_0_0.bin"
/usr/bin/install -o root -g wheel -m 644 "$here/org.nullarc.vcn-ready.plist" "$plist"
/bin/launchctl bootstrap system "$plist"
