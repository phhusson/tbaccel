#!/bin/sh
# Install the minimal test kext (TbAccelMin: one trivial class, no personalities)
# and remove the real one, to bisect whether the kernel panic is the load path or
# our kext's classes/personalities.
set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$HERE/build_min/TbAccelMin.kext"
DEST="/Library/Extensions/TbAccelMin.kext"

[ -d "$SRC" ] || { echo "no $SRC - build target TbAccelMin first" >&2; exit 1; }

# make sure the real (panicking) kext is not present
sudo rm -rf /Library/Extensions/TbAccel.kext
sudo rm -rf "$DEST"
sudo cp -R "$SRC" "$DEST"
sudo chown -R root:wheel "$DEST"
sudo chmod -R go-w "$DEST"

echo "== staging TbAccelMin =="
sudo kmutil load -z -p "$DEST" --load-style start-only
echo "installed $DEST ; REBOOT: sudo reboot"
