#!/bin/sh
# Invoked serially by the watcher, under launchd's system domain.
set -eu
base=/Library/PrivilegedHelperTools/org.nullarc.vcn-ready
gpu=/Users/Shared/gpu
kext=/Library/Extensions/PhoenixVALoadProbe.kext
reason=${1:-startup}
if [ -e "$gpu/phxboot-off" ]; then exit 0; fi

inspect() { /usr/bin/python3 -I -B "$base/registry.py"; }
before=$(inspect)
case "$before" in
    skip) exit 0 ;;
    'ready '*|'missing '*) entry=${before#* } ;;
    *) echo "VCN $reason: conflicting discovery state ($before)" >&2; exit 1 ;;
esac

state=$("$gpu/vcnctl" status)
case "$(printf '%s\n' "$state" | /usr/bin/head -1)" in
    'VCN: up') echo "VCN $reason: already up" ;;
    'VCN: down')
        echo "VCN $reason: initializing"
        "$gpu/vcnctl" init "$base/vcn_4_0_0.bin"
        state=$("$gpu/vcnctl" status)
        [ "$(printf '%s\n' "$state" | /usr/bin/head -1)" = 'VCN: up' ] ;;
    *) echo "VCN $reason: unknown status" >&2; exit 1 ;;
esac

case "$(inspect)" in
    "ready $entry") echo "Phoenix $reason: already attached to NMXAccel $entry"; exit 0 ;;
    "missing $entry") ;;
    *) echo "Phoenix $reason: device or discovery state changed before load" >&2; exit 1 ;;
esac

# kmutil uses normal macOS approval. Its success alone does not establish attachment.
rc=0
result=$(LC_ALL=C /usr/bin/kmutil load -p "$kext" 2>&1) || rc=$?
[ -z "$result" ] || printf '%s\n' "$result" >&2
case "$(inspect)" in
    "ready $entry") echo "Phoenix $reason: attached to NMXAccel $entry after load"; exit 0 ;;
    "missing $entry") ;;
    *) echo "Phoenix $reason: device or discovery state changed during load" >&2; exit 1 ;;
esac
case "$result" in
    *'Code=27 '*|*'not approved to load'*|*'approve using System Settings'*)
        echo "Phoenix $reason: awaiting approval in System Settings" >&2; exit 0 ;;
    *'requires a reboot'*|*'requires reboot'*)
        echo "Phoenix $reason: macOS reports a restart is required" >&2; exit 0 ;;
esac
if [ "$rc" -ne 0 ]; then exit "$rc"; fi
echo "Phoenix $reason: request accepted, awaiting attachment to NMXAccel $entry"
