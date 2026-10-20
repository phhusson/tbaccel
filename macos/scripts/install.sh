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

# Approvals are per CDHash, and a rebuilt kext has a new one. Seed the User
# Authorized Kext List before staging, otherwise kernelmanagerd refuses the
# collection build and the *previous* build silently keeps booting.
if [ -x "$HERE/scripts/approve.sh" ]; then
    "$HERE/scripts/approve.sh" || true
fi

echo "== staging + rebuilding auxiliary kext collection =="
# -z skips the per-build System Settings approval (dev machine).
sudo kmutil load -z -p "$DEST" --load-style start-only || true

# The collection build can silently keep the previous kext (approval missing,
# pre-flight failure, or a stale stash) while kmutil still prints "requires a
# reboot". Verify the *built* collection actually contains this build: the tag
# is compiled in, so a string search settles it.
TAG=$(sed -n 's/.*TBACCEL_BUILD_TAG "\(.*\)".*/\1/p' "$HERE/Kernel/TbAccelInternal.h" | head -1)
sleep 3
KC=$(ls /System/Volumes/Preboot/*/boot/*/System/Library/Caches/com.apple.kernelcaches/kernelcache.auxkc.* 2>/dev/null | head -1)
if [ -n "$TAG" ] && [ -n "$KC" ] && sudo strings -a "$KC" 2>/dev/null | grep -qx "$TAG"; then
    echo "collection contains $TAG - OK"
else
    echo
    echo "*** STAGED COLLECTION DOES NOT CONTAIN $TAG ***"
    echo "    the build was refused; approve the kext in System Settings and rerun."
    echo "    (kmutil log show | grep -E 'not approved|failed to link')"
fi

echo
echo "installed $DEST"
echo "REBOOT to load it:  sudo reboot"
