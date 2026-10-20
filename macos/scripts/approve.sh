#!/bin/sh
# Seed macOS's User Authorized Kext List with the currently-installed TbAccel
# CDHash so the next boot accepts it without a System Settings prompt.
#
#   sudo scripts/approve.sh
#   sudo reboot
#
# (SIP is disabled on this dev box; on a stock machine use the GUI prompt.)
set -e

K=/Library/Extensions/TbAccel.kext
[ -d "$K" ] || { echo "no $K" >&2; exit 1; }

CD=$(codesign -dvvv "$K" 2>&1 | sed -n 's/.*CDHash=\([0-9a-f]*\).*/\1/p' | head -1)
CUR=$(sysctl -n kern.bootsessionuuid)
echo "authorizing com.phh.tbaccel cdhash=$CD bootsession=$CUR"

sudo python3 - "$CD" "$CUR" <<'PY'
import plistlib, sys
cd, cur = sys.argv[1], sys.argv[2]
p = "/var/db/SystemPolicyConfiguration/UAKL.plist"
d = plistlib.load(open(p, "rb"))
d["BootSession"] = cur
arr = d.setdefault("SignedKernelExtensions", {}).setdefault("TBACCEL001", [])
if ["com.phh.tbaccel", cd] not in arr:
    arr.append(["com.phh.tbaccel", cd])
plistlib.dump(d, open(p, "wb"))
print("UAKL entries:", len(arr))
PY

# clear any stale 'pending upgrade' flags so the prompt/blocker doesn't form
sudo sqlite3 /var/db/SystemPolicyConfiguration/KextPolicy \
    "update kext_load_history_v3 set flags=16 where bundle_id='com.phh.tbaccel';" 2>/dev/null || true
sqlite3 /var/db/SystemPolicyConfiguration/KextPolicy \
    "delete from kext_load_history_v3 where bundle_id='com.phh.tbaccel.min';" 2>/dev/null || true

echo "done - reboot to load"
