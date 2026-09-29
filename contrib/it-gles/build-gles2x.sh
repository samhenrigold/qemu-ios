#!/bin/bash
# The 1.x/2.x OpenGLES.framework/OpenGLES replacement (gles2x.c over the mbxshim core), legacy-linked for
# the 1.x/2.x dyld: its export set is the firmware's own (opengles-<os>.exports, gles2x_exports.py), and it
# is slid like the dylib it replaces (mkold.py --legacy turns its rebases into classic local relocations).
#   build-gles2x.sh [OUT]        2.x: OUT (default contrib/it-gles/OpenGLES-2x, next to MBXGLEngine)
#   build-gles2x.sh 1x [OUT]     1.x: the same core without EAGL (GLES2X_EAGL=0), plain C over libSystem
#                                (1.x Objective-C is the old ABI), OUT default contrib/it-gles/OpenGLES-1x
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
OS=2x
if [ "${1:-}" = 1x ] || [ "${1:-}" = 2x ]; then OS="$1"; shift; fi
export LEGACY_LINK=1 ARMV6_SDK="${ARMV6_SDK:-$HOME/Developer/ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk}"
. "$HERE/../armv6-toolchain/armv6.sh"
OUT="${1:-$HERE/OpenGLES-$OS}"
GEN="$(mktemp -d /tmp/gles2x.XXXXXX)"
trap 'rm -rf "$GEN"' EXIT
python3 "$HERE/genstubs.py" "$HERE/gles_stubs.h" >/dev/null
python3 "$HERE/gles2x_exports.py" gen "$HERE/opengles-$OS.exports" \
    "$HERE/../../include/hw/arm/guest-services/gles-names.h" "$GEN"
if [ "$OS" = 1x ]; then
    cc6 "$HERE/gles2x.c" "$GEN/gles2x.o" -I"$GEN" -DGLES2X_EAGL=0
    link6 -dylib "$OUT" "$GEN/gles2x.o" -install_name /System/Library/Frameworks/OpenGLES.framework/OpenGLES \
        -compatibility_version 1.0 -current_version 1.0 -exported_symbols_list "$GEN/gles2x.exp"
    # no ldid: 1.x predates code signing
    file "$OUT"
    exit 0
fi
cc6 "$HERE/gles2x.c" "$GEN/gles2x.o" -x objective-c -I"$GEN" -Wno-objc-root-class
# What the EAGL classes need beyond libSystem, bound two-level from the library 2.x has it in (the
# 2.0 SDK: NSObject and the constant-string class in CoreFoundation, the messengers in libobjc), so a
# bare dlopen resolves them as it does stock OpenGLES's. ld takes no framework stub from an SDK this
# old ("built for 'unknown'"), so each is a text stub naming only these: a new undefined symbol fails
# the link here, not the load on the device.
tbd() {   # tbd FILE INSTALL_NAME CURRENT COMPAT SYMBOLS OBJC_CLASSES
    printf -- "--- !tapi-tbd\ntbd-version: 4\ntargets: [ armv7-ios ]\ninstall-name: '%s'\ncurrent-version: %s\ncompatibility-version: %s\nexports:\n  - targets: [ armv7-ios ]\n    symbols: [ %s ]\n    objc-classes: [ %s ]\n...\n" \
        "$2" "$3" "$4" "$5" "$6" >"$1"
}
tbd "$GEN/CoreFoundation.tbd" /System/Library/Frameworks/CoreFoundation.framework/CoreFoundation 478.23 150 \
    ___CFConstantStringClassReference NSObject
tbd "$GEN/libobjc.tbd" /usr/lib/libobjc.A.dylib 227 1 "_objc_msgSend, _objc_msgSendSuper2, __objc_empty_cache" ""
# Nothing binds from Foundation, but it must be loaded as the stock OpenGLES loads it: CoreFoundation's
# NSObject -retain/-release go through the refcount table Foundation installs (without it a bare
# dlopen's first +setCurrentContext: never returns).
tbd "$GEN/Foundation.tbd" /System/Library/Frameworks/Foundation.framework/Foundation 678.24 300 "" ""
link6 -dylib "$OUT" "$GEN/gles2x.o" -install_name /System/Library/Frameworks/OpenGLES.framework/OpenGLES \
    -compatibility_version 1.0 -current_version 1.0 -exported_symbols_list "$GEN/gles2x.exp" \
    "$GEN/CoreFoundation.tbd" "$GEN/libobjc.tbd" "$GEN/Foundation.tbd"
if command -v ldid >/dev/null; then ldid -S "$OUT"; fi
file "$OUT"
