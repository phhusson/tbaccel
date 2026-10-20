#!/bin/sh
# Install/load TbAccel.kext for the next boot.
#
#   scripts/install.sh
#   sudo reboot
#
# On Apple Silicon a kext only loads from the Auxiliary Kernel Collection, which
# kernelmanagerd builds for the next boot. The supported way to trigger that is
# `kmutil load -p <kext>` (which stages + validates + rebuilds the aux KC).
# `kmutil rebuild` / `install --update-all` build from a manifest that doesn't
# know about our kext (and --update-all also tries the boot collection, which
# fails against the x86_64 /System/Library/Kernels/kernel stub), so we don't use
# those.
set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"
# Prefer the Xcode-built kext (correct module glue); fall back to make's output.
KEXT="$HERE/build_xc/TbAccel.kext"
[ -d "$KEXT" ] || KEXT="$HERE/build/TbAccel.kext"
DEST="/Library/Extensions/TbAccel.kext"

if [ ! -d "$KEXT" ]; then
    echo "no $KEXT - run 'make' in $HERE first" >&2
    exit 1
fi

sudo rm -rf "$DEST"
sudo cp -R "$KEXT" "$DEST"
sudo chown -R root:wheel "$DEST"
sudo chmod -R go-w "$DEST"

echo "== staging + rebuilding auxiliary kext collection =="
# -z skips the per-build System Settings approval (dev machine).
sudo kmutil load -z -p "$DEST" --load-style start-only

echo
echo "installed $DEST"
echo "REBOOT to load it:  sudo reboot"
