#!/bin/bash
# it_prefs for the iPod touch 2G (armv6, 3.1.3 and 4.x): only SpringBoard's first-run tip
# (IT_PREFS_TIP_ONLY). Output: build/ipod-guest/it_prefs, ldid-signed (4.x AMFI wants one);
# imgtools/ipod2g_device.py bakes it with com.qemu.it-prefs.plist.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
OUT="$HERE/../../build/ipod-guest"
mkdir -p "$OUT"
cc6 "$HERE/it_prefs.c" "$OUT/it_prefs.o" -DIT_PREFS_TIP_ONLY
link6 -execute "$OUT/it_prefs" "$OUT/it_prefs.o"
rm -f "$OUT/it_prefs.o"
"${LDID:-ldid}" -S "$OUT/it_prefs"
file "$OUT/it_prefs"
