#!/bin/bash
# The one iPod guest tool the iPad keeps: it_pbd, the pasteboard bridge (no stock
# 3.2.2 service reaches the pasteboard). Everything else is a stock USB service or
# a hardware model -- docs/ipad1/guest-services.md. Same source, armv6-toolchain
# pipeline with GUEST_ARCH=armv7 against the 3.2 SDK. Output: build/ipad1-guest/
# (untracked), installed by `ipad1_rootfs.py bake`.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../build/ipad1-guest}"
export GUEST_ARCH=armv7
export ARMV6_SDK="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"
. "$HERE/../armv6-toolchain/armv6.sh"
mkdir -p "$OUT"
cc6 "$HERE/../it-pasteboard/it_pbd.c" "$OUT/it_pbd.o"
link6 -execute "$OUT/it_pbd" "$OUT/it_pbd.o"
rm -f "$OUT/it_pbd.o"
"${LDID:-ldid}" -S "$OUT/it_pbd"
file "$OUT/it_pbd"
