#!/bin/sh
# Remove the persistent TbAccel.kext and rebuild the collections.
set -e

sudo rm -rf /Library/Extensions/TbAccel.kext
sudo kmutil rebuild
echo "removed /Library/Extensions/TbAccel.kext; reboot to fully unload"
