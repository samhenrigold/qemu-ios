#!/bin/bash
# A new emulated iPad from declared inputs only, booted twice on one overlay.
#
#   tests/ipad1/fresh-device.sh [MANIFEST] [OUT] [-- create options, e.g. --activation-hook SCRIPT]
#
# 1. imgtools/ipad1_device.py create MANIFEST OUT/device (skipped if OUT/device/device.lock.json exists)
# 2. device.lock.json names no hw2/ dump and no identity.json but the device's own
# 3. boot 1: no FTL rescan (the store is sealed), lit lock screen, unlock, OUT/boot1.png, clean power-off
#    (QEMU exit 0 within 45 s). A device made without an activation hook stops at "Connect to iTunes":
#    then lit means that screen and there is nothing to unlock.
# 4. boot 2 on the same overlay: the same; no rescan now also means boot 1's power-off closed the FTL
# Pass/fail is per step; whether a PNG shows the home screen or "Connect to iTunes" needs a look
# (brightness alone cannot tell them apart).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MANIFEST="${1:-$ROOT/manifests/ipad1-7B500.json}"
OUT="${2:-$(mktemp -d "$HOME/Developer/qemu-ios-files/ipad1/repro/fresh.XXXX")}"
shift $(( $# < 2 ? $# : 2 )); [ "${1:-}" = "--" ] && shift
DEV="$OUT/device"
mkdir -p "$OUT"

[ -f "$DEV/device.lock.json" ] || python3 "$ROOT/imgtools/ipad1_device.py" create "$MANIFEST" "$DEV" "$@"

DIE_ID=$(python3 - "$DEV" <<'EOF'
import json, os, sys
dev = sys.argv[1]
lock = json.load(open(os.path.join(dev, "device.lock.json")))
paths = []
def walk(v):
    if isinstance(v, dict):
        for x in v.values(): walk(x)
    elif isinstance(v, list):
        for x in v: walk(x)
    elif isinstance(v, str):
        paths.append(v)
walk(lock["inputs"])
bad = [p for p in paths if "/hw2/" in p or (p.endswith("identity.json") and p != os.path.join(dev, "identity.json"))]
assert not bad, "lock inputs reference unit data: %s" % bad
assert lock["inputs"]["lockdown"] is None and lock["inputs"]["stash"] is None
print(lock["identity"]["die_id"], "activated" if lock["inputs"]["activation_hook"] else "itunes")
EOF
)
read -r DIE_ID SCREEN <<<"$DIE_ID"
if [ "$SCREEN" = activated ]; then SCREEN_FLAGS=(--unlock); WHAT="lit, unlocked"
else SCREEN_FLAGS=(--lit 2500); WHAT="lit (Connect to iTunes)"; fi
echo "PASS lock: inputs name no hw2/ dump and only $DEV/identity.json"

# The sealed nor.bin is read-only; boot both times on one private writable copy so
# the effaceable region (and the system keybag it protects) persists across boots.
NOR_FLAGS=()
if [ -f "$DEV/nor.bin" ]; then
    cp "$DEV/nor.bin" "$OUT/nor.bin"; chmod u+w "$OUT/nor.bin"
    NOR_FLAGS=(--nor-rw "$OUT/nor.bin")
fi

for n in 1 2; do
    flags=(--nand-overlay "$DEV/nand" --overlay "$OUT/overlay" --kboot "$DEV/kboot.bin"
           --die-id "$DIE_ID" ${NOR_FLAGS[@]+"${NOR_FLAGS[@]}"} "${SCREEN_FLAGS[@]}" --shot "$OUT/boot$n.png" --powerdown --no-rescan --seconds 240)
    if timeout 300 python3 "$ROOT/tests/ipad1/boot-smoke.py" "${flags[@]}" > "$OUT/boot$n.txt" 2>&1; then
        echo "PASS boot $n: $WHAT, clean power-off; look at $OUT/boot$n.png"
    else
        echo "FAIL boot $n: see $OUT/boot$n.txt"; cat "$OUT/boot$n.txt"; exit 1
    fi
done
