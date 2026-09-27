#!/bin/bash
# Build the AppSync dylib as one fat armv6+armv7 Mach-O, ldid-signed.
#
#   armv6 slice -> iPod touch 2G, iOS 3.1.3 (3.1.3 SDK)
#   armv7 slice -> iPad 1,       iOS 3.2.2 (3.2 SDK)
#
# Both slices come from the same appsync.c through the armv6-toolchain pipeline
# (clang -marm, ld-as-armv7, mkold.py strips modern load commands). Output:
# build/appsync/libappsync.dylib (untracked), installed by ipad1_rootfs.py
# --appsync and the iPod baker's --appsync-dylib option.
#
#   build.sh [OUTDIR]
#   build.sh --regen-cert   # make a fresh signer cert (rarely needed)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"

if [ "${1:-}" = "--regen-cert" ]; then
    # SHA-1 / RSA-1024: iOS 3.2.2's SecCertificateCreateWithData rejects modern
    # SHA-256/RSA-2048 DERs (installd logs "Could not create certificate from data").
    openssl req -x509 -newkey rsa:1024 -sha1 -nodes -keyout /dev/null \
        -out /tmp/appsync.der -outform DER -days 36500 \
        -subj "/CN=LightTouch AppSync/O=LightTouch Emulator"
    python3 - "$HERE/appsync_cert.h" <<'PY'
import sys
b=open('/tmp/appsync.der','rb').read()
out=['// Generated: self-signed DER, any cert satisfies installd\'s signer-summary read.',
     '// Regenerate with build.sh --regen-cert. Not per-firmware.',
     'static const unsigned char kAppSyncCertDER[] = {']
for i in range(0,len(b),12):
    out.append('    '+''.join('0x%02x, '%c for c in b[i:i+12]).rstrip())
out+=['};','static const unsigned int kAppSyncCertDERLen = %d;'%len(b)]
open(sys.argv[1],'w').write('\n'.join(out)+'\n')
print('regenerated appsync_cert.h', len(b), 'bytes')
PY
    exit 0
fi

OUT="${1:-$HERE/../../build/appsync}"
mkdir -p "$OUT"

build_slice() {  # arch  sdk  out
    local arch="$1" sdk="$2" out="$3"
    ( export GUEST_ARCH="$arch" ARMV6_SDK="$sdk"
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

lipo -create "$OUT/libappsync.armv6" "$OUT/libappsync.armv7" \
     -output "$OUT/libappsync.dylib"
rm -f "$OUT/libappsync.armv6" "$OUT/libappsync.armv7"

"${LDID:-ldid}" -S "$OUT/libappsync.dylib"
file "$OUT/libappsync.dylib"
lipo -detailed_info "$OUT/libappsync.dylib" | grep -E 'architecture|cputype|offset' || true
