#!/bin/bash
# Run the NullArc installer (apps-only mode, no --windowserver) from a copy that macOS unpacks itself, with a full
# trace. A copy written from Linux (hfsplus) carries com.apple.FinderInfo on some files, which makes the installer's
# `codesign --force --sign -` fail and stop before the LaunchDaemon is installed.
# Run in macOS:  sudo bash 1-install.sh /path/to/nullarc-0.4.0-alpha.zip
# Output: /Users/Shared/f7bsc-logs/phx-install-trace-<time>.txt
L=/Users/Shared/f7bsc-logs; mkdir -p "$L"
ZIP="${1:?usage: sudo bash 1-install.sh /path/to/nullarc-0.4.0-alpha.zip}"
WANT=dcfeaa04917719683487b24259933b6ea84513a840cc862a68cd91205af47bbb
WORK=/private/tmp/nullarc-0.4.0-install
OUT="$L/phx-install-trace-$(date +%Y%m%d-%H%M%S).txt"
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }
export LANG=en_US.UTF-8 LC_ALL=en_US.UTF-8
{
echo "===== zip"
GOT=$(shasum -a 256 "$ZIP" | awk '{print $1}'); echo "sha256 $GOT"
[ "$GOT" = "$WANT" ] || { echo "zip checksum mismatch, stopping"; exit 1; }
rm -rf "$WORK"; mkdir -p "$WORK"
ditto -x -k "$ZIP" "$WORK" || { echo "unzip failed"; exit 1; }
SRC="$WORK/nullarc-0.4.0-alpha"
xattr -cr "$SRC"
echo "files still carrying xattrs after xattr -cr:"; find "$SRC" -exec sh -c '[ -n "$(xattr "$1" 2>/dev/null)" ] && echo "  $1"' _ {} \;
echo "===== install.sh (bash -x)"
cd "$SRC" && bash -x ./install.sh
echo "===== install.sh exit code: $?"
echo "===== post-install state"
ls -la /Library/LaunchDaemons/local.phxboot.plist /Library/GPUBundles /Library/GPUBundles/nvmtl-allow.txt 2>&1
H=/System/Library/CoreServices/CoreTypes.bundle/Contents/Library/PhxGPU
ls -la "$H" 2>&1
echo "--- codesign verify"
codesign --verify --strict --verbose=4 "$H/NVMTLDriver.bundle" 2>&1
echo "--- aux KC"
kmutil inspect -a x86_64 -A /Library/KernelCollections/AuxiliaryKernelExtensions.kc 2>&1 | grep -iE 'luuumi|collection'
echo "--- launchd"
plutil -lint /Library/LaunchDaemons/local.phxboot.plist 2>&1
ls -la /Users/Shared/gpu/phxboot-m2 2>&1
} > "$OUT" 2>&1
sync
echo "trace: $OUT"
grep -E 'exit code|checksum|local.phxboot.plist|satisfies|valid on disk|detritus' "$OUT"
