#!/bin/sh
# Load/unload TbAccel.kext for development.
#
#   make                 # build as yourself (codesign needs your keychain)
#   scripts/load.sh      # load (no reboot)
#   scripts/load.sh unload
#
# Notes:
#  * Each rebuild changes the CDHash, so macOS wants a fresh "allow" in
#    System Settings. `kmutil load -z` (--no-authorization) skips that, which
#    is what makes no-reboot iteration possible on a dev machine.
#  * kmutil validates ownership, so we stage a root:wheel copy and hand *that*
#    to kmutil (never build/, which stays yours so `make` keeps working).
set -e

HERE="$(cd "$(dirname "$0")/.." && pwd)"
KEXT="$HERE/build/TbAccel.kext"
ID="com.phh.tbaccel"

if [ "$1" = "unload" ]; then
    sudo kmutil unload -b "$ID" 2>/dev/null || sudo kextunload -b "$ID" 2>/dev/null || true
    echo "unloaded $ID"
    exit 0
fi

if [ ! -d "$KEXT" ]; then
    echo "no $KEXT - run 'make' in $HERE first" >&2
    exit 1
fi

sudo kmutil unload -b "$ID" 2>/dev/null || true

STAGE="$(mktemp -d)/TbAccel.kext"
cp -R "$KEXT" "$STAGE"
sudo chown -R root:wheel "$STAGE"

sudo kmutil load -z -p "$STAGE" --load-style start-only

echo "== loaded: =="
kmutil showloaded 2>/dev/null | grep -i tbaccel || echo "(not in showloaded yet)"
echo "== logs: =="
echo "   log stream --predicate 'process == \"kernel\"' | grep -iE 'TbAccel|accel'"
