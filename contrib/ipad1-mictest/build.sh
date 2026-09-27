#!/bin/bash
# Test-only capture probe for the ipad1 machine (see it_mictest.c). Output:
# build/ipad1-mictest/it_mictest, ldid ad-hoc signed (needs the AMFI boot-args).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../build/ipad1-mictest}"
export GUEST_ARCH=armv7
export ARMV6_SDK="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"
. "$HERE/../armv6-toolchain/armv6.sh"
mkdir -p "$OUT"
cc6 "$HERE/it_mictest.c" "$OUT/it_mictest.o" -F"$ARMV6_SDK/System/Library/Frameworks" \
    -isystem "$(xcrun clang -print-resource-dir)/include" -D_FORTIFY_SOURCE=0
link6 -execute "$OUT/it_mictest" "$OUT/it_mictest.o"
rm -f "$OUT/it_mictest.o"
"${LDID:-ldid}" -S "$OUT/it_mictest"
file "$OUT/it_mictest"
