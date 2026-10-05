#!/bin/bash
# Build it_boot, the guest-package loader, for both guest architectures:
#   OUT/armv6/it_boot  iPod (n72), legacy-linked so 2.x dyld loads it too
#   OUT/armv7/it_boot  iPad (k48)
#   OUT/armv7-legacy/it_boot  armv7 on 3.0 (k48-ios30), legacy-linked against the 3.1.3 SDK for 3.0's dyld
# libSystem only, ldid-signed. Then the host test (fake qc(), ASan/UBSan).
#   build.sh [OUT]      default build/it-boot (untracked)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="${1:-$HERE/../../build/it-boot}"
I6_SDK="${ARMV6_SDK:-$HOME/Developer/ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk}"
I7_SDK="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"
for v in armv6:"$I6_SDK":1:armv6 armv7:"$I7_SDK":0:armv7 armv7:"$I6_SDK":1:armv7-legacy; do
    IFS=: read -r arch sdk legacy dir <<<"$v"
    mkdir -p "$OUT/$dir"
    ( export GUEST_ARCH="$arch" ARMV6_SDK="$sdk" LEGACY_LINK="$legacy"
      . "$HERE/../armv6-toolchain/armv6.sh"
      # -D_FORTIFY_SOURCE=0: 3.2's libSystem has no __snprintf_chk
      cc6 "$HERE/it_boot.c" "$OUT/$dir/it_boot.o" -Wall -D_FORTIFY_SOURCE=0
      link6 -execute "$OUT/$dir/it_boot" "$OUT/$dir/it_boot.o"
      rm -f "$OUT/$dir/it_boot.o" )
    "${LDID:-ldid}" -S "$OUT/$dir/it_boot"
    cp "$HERE/com.qemu.it-boot.plist" "$OUT/$dir/"
    file "$OUT/$dir/it_boot"
done
python3 "$HERE/../../tests/guest-package/test_it_boot.py"
