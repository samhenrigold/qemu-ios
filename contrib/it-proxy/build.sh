#!/bin/bash
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
cc6 "$HERE/itproxy.c" "$HERE/itproxy.o"
link6 -execute "$HERE/itproxy" "$HERE/itproxy.o" -e __start
rm -f "$HERE/itproxy.o"
# httpget runs on 2.x too (sessions single's Wi-Fi check): a legacy link (2.x dyld refuses LC_DYLD_INFO_ONLY) and
# a signature (2.x kernels kill unsigned pages; cs_enforcement_disable is a boot-arg only from 3.0).
LEGACY_LINK=1 cc6 "$HERE/httpget.c" "$HERE/httpget.o"
LEGACY_LINK=1 link6 -execute "$HERE/httpget" "$HERE/httpget.o"
rm -f "$HERE/httpget.o"
"${LDID:-ldid}" -S "$HERE/httpget"
# iOS 10 (the iPhone 5's) runs no armv6 code: the same client for armv7, linked normally.
GUEST_ARCH=armv7 cc6 "$HERE/httpget.c" "$HERE/httpget-armv7.o"
GUEST_ARCH=armv7 link6 -execute "$HERE/httpget-armv7" "$HERE/httpget-armv7.o"
rm -f "$HERE/httpget-armv7.o"
"${LDID:-ldid}" -S "$HERE/httpget-armv7"

cc6 "$HERE/ittrust.c" "$HERE/ittrust.o"
link6 -execute "$HERE/ittrust" "$HERE/ittrust.o" -e __start
rm -f "$HERE/ittrust.o"

"${LDID:-ldid}" -S"$HERE/ittrust.entitlements" "$HERE/ittrust"
