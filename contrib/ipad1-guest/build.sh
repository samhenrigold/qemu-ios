#!/bin/bash
# The iPad's guest helpers: it_pbd, the pasteboard bridge (no stock 3.2.2 service
# reaches the pasteboard), and it_ethlink, which raises the USB Ethernet link the
# way tethering would. Everything else is a stock USB service or a hardware
# model -- docs/ipad1/guest-services.md. Same source, armv6-toolchain
# pipeline with GUEST_ARCH=armv7 against the 3.2 SDK. it_keybag is not baked: it is the
# 4.x restore-ramdisk one-shot (imgtools/ipad1_keybag.py). Output: build/ipad1-guest/
# (untracked), installed by `ipad1_rootfs.py bake`.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../build/ipad1-guest}"
export GUEST_ARCH=armv7
export ARMV6_SDK="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"
. "$HERE/../armv6-toolchain/armv6.sh"
mkdir -p "$OUT"
for src in it-pasteboard/it_pbd it-ethlink/it_ethlink it-seal/it_seal it-prefs/it_prefs it-keybag/it_keybag; do
    t="${src##*/}"
    cc6 "$HERE/../$src.c" "$OUT/$t.o"
    link6 -execute "$OUT/$t" "$OUT/$t.o"
    rm -f "$OUT/$t.o"
    ents="$HERE/../${src%/*}/$t-entitlements.xml"
    "${LDID:-ldid}" -S$([ -f "$ents" ] && echo "$ents") "$OUT/$t"
    file "$OUT/$t"
done
# it_agent: the iPod's guest agent (foreground app, lock state, launch, sync, the pasteboard), as
# contrib/it-agent/build.sh builds it: clang's own stdarg.h, and SpringBoardServices' entitlements.
cc6 "$HERE/../it-agent/it_agent.c" "$OUT/it_agent.o" -isystem "$(xcrun clang -print-resource-dir)/include"
link6 -execute "$OUT/it_agent" "$OUT/it_agent.o"
rm -f "$OUT/it_agent.o"
"${LDID:-ldid}" -S"$HERE/../it-gles/sblaunch-entitlements.xml" "$OUT/it_agent"
file "$OUT/it_agent"
# it_heading: the compass probe (contrib/it-heading), not baked by default.
# -D_FORTIFY_SOURCE=0: 3.2's libSystem has no __vsnprintf_chk.
cc6 "$HERE/../it-heading/it_heading.c" "$OUT/it_heading.o" -D_FORTIFY_SOURCE=0
link6 -execute "$OUT/it_heading" "$OUT/it_heading.o"
rm -f "$OUT/it_heading.o"
"${LDID:-ldid}" -S "$OUT/it_heading"
file "$OUT/it_heading"
# it_cctest: CommonCrypto known answers on the guest CPU (contrib/it-cctest), not baked by default.
cc6 "$HERE/../it-cctest/it_cctest.c" "$OUT/it_cctest.o"
link6 -execute "$OUT/it_cctest" "$OUT/it_cctest.o"
rm -f "$OUT/it_cctest.o"
"${LDID:-ldid}" -S "$OUT/it_cctest"
file "$OUT/it_cctest"
# it_gltest: the GL fixture job (contrib/it-gltest), baked only by `bake --gl-test`.
cc6 "$HERE/../it-gltest/it_gltest.c" "$OUT/it_gltest.o"
link6 -execute "$OUT/it_gltest" "$OUT/it_gltest.o"
rm -f "$OUT/it_gltest.o"
"${LDID:-ldid}" -S "$OUT/it_gltest"
file "$OUT/it_gltest"
# it_msmquiet.dylib: DYLD_INSERT_LIBRARIES into MobileStorageMounter (see its source).
# CF symbols bind flat at load time: the 3.2 SDK's CF stub won't link under modern ld64.
cc6 "$HERE/../it-msmquiet/it_msmquiet.c" "$OUT/it_msmquiet.o"
link6 -dylib "$OUT/it_msmquiet.dylib" "$OUT/it_msmquiet.o" -undefined dynamic_lookup \
    -install_name /usr/local/lib/it_msmquiet.dylib
rm -f "$OUT/it_msmquiet.o"
"${LDID:-ldid}" -S "$OUT/it_msmquiet.dylib"
file "$OUT/it_msmquiet.dylib"
