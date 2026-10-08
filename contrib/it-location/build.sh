#!/bin/bash
# it_location, the CoreLocation probe (it_location.c): armv6, so every guest runs it. Output beside the source
# (untracked), where LightTouchMac's sessions check takes it from the pinned checkout.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/it_location.c" "$HERE/it_location.o"
link6 -execute "$HERE/it_location" "$HERE/it_location.o" -e __start
rm -f "$HERE/it_location.o"
"${LDID:-ldid}" -S"$HERE/it_location.entitlements" "$HERE/it_location"
