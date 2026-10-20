#!/bin/sh
# Unload TbAccel.kext (also available as: scripts/load.sh unload).
set -e
sudo kextunload -b com.phh.tbaccel 2>/dev/null || \
    sudo kmutil unload -b com.phh.tbaccel
echo "unloaded com.phh.tbaccel"
