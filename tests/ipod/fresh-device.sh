#!/bin/bash
# A new emulated iPod from declared inputs only: imgtools/device.py create, then the default regression
# tier on it (boot, fsck, persist = boot -> home -> clean power-off -> boot -> home, appinstall, applaunch,
# gles, agent, audio), one regress.py run per check group so no call runs long.
#
#   tests/ipod/fresh-device.sh [MANIFEST] [OUT] [-- create options]
#
# The iPad's counterpart is tests/ipad1/fresh-device.sh; both read the same device.lock.json.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
MANIFEST="${1:-$ROOT/manifests/ipod2g-7E18.json}"
OUT="${2:-$(mktemp -d "$HOME/Developer/qemu-ios-files/ipod-ipsw/fresh.XXXX")}"
shift $(( $# < 2 ? $# : 2 )); [ "${1:-}" = "--" ] && shift
DEV="$OUT/device"
mkdir -p "$OUT"
[ -f "$DEV/device.lock.json" ] || python3 "$ROOT/imgtools/device.py" create "$MANIFEST" "$DEV" "$@"
python3 - "$DEV" <<'PY'
import json, os, sys
lock = json.load(open(os.path.join(sys.argv[1], "device.lock.json")))
assert lock["board"] == "n72ap" and lock["inputs"]["lockdown"] is None, "not a fresh iPod device"
print("PASS lock: %s %s, UDID %s" % (lock["product_type"], lock["build"], lock["identity"]["udid"]))
PY
# this boot's offer: the seed's own package again (so it_boot stays and reports it)
PKG_FLAGS=()
if OFFER=$(python3 - "$DEV" "$OUT" "$ROOT" <<'PY'
import json, os, sys
dev, out, root = sys.argv[1:]
sys.path.insert(0, os.path.join(root, "contrib/guest-package"))
import mkpkg
lock = json.load(open(os.path.join(dev, "device.lock.json")))
g = lock.get("guest_package") or sys.exit(1)
mkpkg.unpack(g["itpack"]["path"], os.path.join(out, "itpack"))
mkpkg.offer(os.path.join(out, "itpack", g["family"]), os.path.join(out, "offer"), lock["build"])
print(os.path.join(out, "offer"))
PY
); then
    PKG_FLAGS=(--guest-package "$OFFER")
    echo "PASS guest package: seed offered from $OFFER"
fi
fail=0
for checks in boot fsck,persist appinstall,applaunch gles,agent,audio; do
    if timeout 590 python3 "$ROOT/tests/ipod/regress.py" --qemu "${QEMU:-$ROOT/build/qemu-system-arm}" \
            --device "$DEV" ${PKG_FLAGS[@]+"${PKG_FLAGS[@]}"} --checks "$checks" --out "$OUT/regress-${checks//,/-}" > "$OUT/regress-${checks//,/-}.txt" 2>&1; then
        echo "PASS $checks"
    else
        echo "FAIL $checks: see $OUT/regress-${checks//,/-}.txt"; fail=1
        [ "$checks" = boot ] && break
    fi
done
exit $fail
