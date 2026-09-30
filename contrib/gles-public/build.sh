#!/bin/bash
# The one OpenGLES.framework/OpenGLES (opengles.c over the mbxshim core): fat armv6 + armv7, legacy-linked for 2.x's
# dyld (contrib/armv6-toolchain LEGACY_LINK: no LC_DYLD_INFO_ONLY, classic relocations, r9 reserved), exporting
# exactly opengles.exports (every name any 2.x-5.x firmware's OpenGLES exports).
#   build.sh [OUT]     default contrib/gles-public/OpenGLES
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
export LEGACY_LINK=1 ARMV6_SDK="${ARMV6_SDK:-$HOME/Developer/ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk}"
. "$HERE/../armv6-toolchain/armv6.sh"
OUT="${1:-$HERE/OpenGLES}"
GEN="$(mktemp -d /tmp/gles-public.XXXXXX)"
trap 'rm -rf "$GEN"' EXIT
python3 "$HERE/../it-gles/genstubs.py" "$HERE/../it-gles/gles_stubs.h" >/dev/null
python3 "$HERE/../it-gles/gles2x_exports.py" gen "$HERE/opengles.exports" \
    "$HERE/../../include/hw/arm/guest-services/gles-names.h" "$GEN"
# What the EAGL classes need beyond libSystem, bound two-level from the library every firmware has it in (NSObject
# and the constant-string class in CoreFoundation, the messengers in libobjc), so a bare dlopen resolves them as it
# does stock OpenGLES's. ld takes no framework stub from an SDK this old ("built for 'unknown'"), so each is a text
# stub naming only these: a new undefined symbol fails the link here, not the load on the device. Foundation binds
# nothing but is loaded as the stock framework loads it: CoreFoundation's NSObject -retain/-release go through the
# refcount table Foundation installs (without it a bare dlopen's first +setCurrentContext: never returns, 2.x).
tbd() {   # tbd FILE INSTALL_NAME CURRENT COMPAT SYMBOLS OBJC_CLASSES
    printf -- "--- !tapi-tbd\ntbd-version: 4\ntargets: [ armv7-ios ]\ninstall-name: '%s'\ncurrent-version: %s\ncompatibility-version: %s\nexports:\n  - targets: [ armv7-ios ]\n    symbols: [ %s ]\n    objc-classes: [ %s ]\n...\n" \
        "$2" "$3" "$4" "$5" "$6" >"$1"
}
tbd "$GEN/CoreFoundation.tbd" /System/Library/Frameworks/CoreFoundation.framework/CoreFoundation 478.23 150 \
    ___CFConstantStringClassReference NSObject
tbd "$GEN/libobjc.tbd" /usr/lib/libobjc.A.dylib 227 1 "_objc_msgSend, _objc_msgSendSuper2, __objc_empty_cache" ""
tbd "$GEN/Foundation.tbd" /System/Library/Frameworks/Foundation.framework/Foundation 678.24 300 "" ""
for GUEST_ARCH in armv6 armv7; do
    cc6 "$HERE/opengles.c" "$GEN/opengles-$GUEST_ARCH.o" -x objective-c -I"$GEN" -Wno-objc-root-class
    link6 -dylib "$GEN/OpenGLES-$GUEST_ARCH" "$GEN/opengles-$GUEST_ARCH.o" \
        -install_name /System/Library/Frameworks/OpenGLES.framework/OpenGLES \
        -compatibility_version 1.0 -current_version 1.0 -exported_symbols_list "$GEN/gles2x.exp" \
        "$GEN/CoreFoundation.tbd" "$GEN/libobjc.tbd" "$GEN/Foundation.tbd"
done
rm -f "$OUT"
lipo -create "$GEN/OpenGLES-armv6" "$GEN/OpenGLES-armv7" -output "$OUT"
if command -v "${LDID:-ldid}" >/dev/null; then "${LDID:-ldid}" -S "$OUT"; fi
# every listed name exported by both slices, nothing else
for a in armv6 armv7; do
    diff <(grep -v '^#' "$HERE/opengles.exports" | sed 's/^/_/' | sort) \
         <(xcrun nm -gU -arch "$a" "$OUT" 2>/dev/null | awk '{print $3}' | sort) >/dev/null ||
        { echo "build.sh: the $a slice does not export exactly opengles.exports" >&2; exit 1; }
done
file "$OUT"
shasum -a 256 "$OUT"
