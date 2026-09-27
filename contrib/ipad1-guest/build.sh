#!/bin/bash
# The iPad's guest helpers: it_pbd, the pasteboard bridge (no stock 3.2.2 service
# reaches the pasteboard), and it_ethlink, which raises the USB Ethernet link the
# way tethering would. Everything else is a stock USB service or a hardware
# model -- docs/ipad1/guest-services.md. Same source, armv6-toolchain
# pipeline with GUEST_ARCH=armv7 against the 3.2 SDK. Output: build/ipad1-guest/
# (untracked), installed by `ipad1_rootfs.py bake`.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../build/ipad1-guest}"
export GUEST_ARCH=armv7
export ARMV6_SDK="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"
. "$HERE/../armv6-toolchain/armv6.sh"
mkdir -p "$OUT"
for src in it-pasteboard/it_pbd it-ethlink/it_ethlink it-seal/it_seal; do
    t="${src##*/}"
    cc6 "$HERE/../$src.c" "$OUT/$t.o"
    link6 -execute "$OUT/$t" "$OUT/$t.o"
    rm -f "$OUT/$t.o"
    "${LDID:-ldid}" -S "$OUT/$t"
    file "$OUT/$t"
done
# it_heading: the compass probe (contrib/it-heading), not baked by default.
# -D_FORTIFY_SOURCE=0: 3.2's libSystem has no __vsnprintf_chk.
cc6 "$HERE/../it-heading/it_heading.c" "$OUT/it_heading.o" -D_FORTIFY_SOURCE=0
link6 -execute "$OUT/it_heading" "$OUT/it_heading.o"
rm -f "$OUT/it_heading.o"
"${LDID:-ldid}" -S "$OUT/it_heading"
file "$OUT/it_heading"
# it_msmquiet.dylib: DYLD_INSERT_LIBRARIES into MobileStorageMounter (see its source).
# CF symbols bind flat at load time: the 3.2 SDK's CF stub won't link under modern ld64.
cc6 "$HERE/../it-msmquiet/it_msmquiet.c" "$OUT/it_msmquiet.o"
link6 -dylib "$OUT/it_msmquiet.dylib" "$OUT/it_msmquiet.o" -undefined dynamic_lookup \
    -install_name /usr/local/lib/it_msmquiet.dylib
rm -f "$OUT/it_msmquiet.o"
"${LDID:-ldid}" -S "$OUT/it_msmquiet.dylib"
file "$OUT/it_msmquiet.dylib"
