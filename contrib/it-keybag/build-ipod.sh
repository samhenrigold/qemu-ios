#!/bin/bash
# it_keybag for the iPod touch 2G (armv6, 4.x): the one volume is disk0s1, data at /private/var.
# Output: build/ipod-guest/it_keybag, ldid-signed. firmwarekit's N72 keybag step boots it.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
OUT="$HERE/../../build/ipod-guest"
mkdir -p "$OUT"
cc6 "$HERE/it_keybag.c" "$OUT/it_keybag.o" -isystem "$(xcrun clang -print-resource-dir)/include" \
    '-DDATA_DEV="/dev/disk0s1"' '-DDATA_MNT="/mnt1"' '-DDATA_DIR="/mnt1/private/var"'
link6 -execute "$OUT/it_keybag" "$OUT/it_keybag.o"
rm -f "$OUT/it_keybag.o"
"${LDID:-ldid}" -S "$OUT/it_keybag"
