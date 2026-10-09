#!/bin/bash
# it_heading as a test probe: armv6, no start delay, 12 s of headings, preauthorized as it_location is.
# Output beside the source (untracked), where LightTouchMac's sessions phone compass check takes it from.
# contrib/ipad1-guest/build.sh builds the iPad's launchd variant (40 s delay, 150 s).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/it_heading.c" "$HERE/it_heading.o" -D_FORTIFY_SOURCE=0 -DDELAY_SECONDS=0 -DRUN_SECONDS=12
link6 -execute "$HERE/it_heading" "$HERE/it_heading.o"
rm -f "$HERE/it_heading.o"
"${LDID:-ldid}" -S"$HERE/../it-location/it_location.entitlements" "$HERE/it_heading"
