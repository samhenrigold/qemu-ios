#!/bin/bash
# Build the AppSync dylib as one fat armv6+armv7 Mach-O, ldid-signed.
#
#   armv6 slice -> iPod touch 2G, iOS 2.x onward (3.1.3 SDK)
#   armv7 slice -> iPad 1,       iOS 3.2.2 (3.2 SDK)
#
# libappsync-legacy.dylib: the same armv6 slice with a legacy-linked armv7 one (3.1.3 SDK) for armv7
# on 3.0, whose dyld refuses LC_DYLD_INFO_ONLY (k48-ios30).
#
# Both slices come from the same appsync.c through the armv6-toolchain pipeline
# (clang -marm, ld-as-armv7, machotool mkold strips modern load commands). Output:
# build/appsync/libappsync.dylib (untracked), which the guest package and FirmwareKit install.
#
#   build.sh [OUTDIR]
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"

OUT="${1:-$HERE/../../build/appsync}"
mkdir -p "$OUT"

build_slice() {  # arch  sdk  out  [legacy]
    local arch="$1" sdk="$2" out="$3"
    ( export GUEST_ARCH="$arch" ARMV6_SDK="$sdk"
      [ "$arch" != armv6 ] && [ -z "${4:-}" ] || export LEGACY_LINK=1
      . "$HERE/../armv6-toolchain/armv6.sh"
      cc6 "$HERE/appsync.c" "$out.o"
      # -dylib with an install name; -undefined dynamic_lookup binds the MIS,
      # CF/Sec and objc symbols in the host process at load. The old-SDK
      # frameworks are "built for unknown" and fatal to link against, so we do
      # not link them -- dynamic_lookup is exactly the escape hatch for that.
      link6 -dylib "$out" "$out.o" \
            -install_name /usr/lib/libappsync.dylib \
            -undefined dynamic_lookup
      rm -f "$out.o" )
}

I6_SDK="${IPOD_SDK:-$HOME/Developer/ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk}"
I7_SDK="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"

build_slice armv6 "$I6_SDK" "$OUT/libappsync.armv6"
build_slice armv7 "$I7_SDK" "$OUT/libappsync.armv7"
build_slice armv7 "$I6_SDK" "$OUT/libappsync.armv7-legacy" legacy

lipo -create "$OUT/libappsync.armv6" "$OUT/libappsync.armv7" \
     -output "$OUT/libappsync.dylib"
lipo -create "$OUT/libappsync.armv6" "$OUT/libappsync.armv7-legacy" \
     -output "$OUT/libappsync-legacy.dylib"
rm -f "$OUT/libappsync.armv6" "$OUT/libappsync.armv7" "$OUT/libappsync.armv7-legacy"

"${LDID:-ldid}" -S "$OUT/libappsync.dylib"
"${LDID:-ldid}" -S "$OUT/libappsync-legacy.dylib"
file "$OUT/libappsync.dylib"
lipo -detailed_info "$OUT/libappsync.dylib" | grep -E 'architecture|cputype|offset' || true

# Early Lockbot forwards ProgramArguments only. This launcher supplies the per-process environment.
( export GUEST_ARCH=armv6 ARMV6_SDK="$I6_SDK" LEGACY_LINK=1
  . "$HERE/../armv6-toolchain/armv6.sh"
  cc6 "$HERE/launcher.c" "$OUT/appsync-launch.o"
  link6 -execute "$OUT/appsync-launch" "$OUT/appsync-launch.o" -e __start
  "${LDID:-ldid}" -S "$OUT/appsync-launch"
  rm "$OUT/appsync-launch.o" )
