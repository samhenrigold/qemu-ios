#!/bin/bash
# The 1.x/2.x OpenGLES.framework/OpenGLES replacement (gles2x.c over the mbxshim core), legacy-linked for
# the 2.x dyld: its export set is the firmware's own (opengles-2x.exports, gles2x_exports.py), and it is
# slid like the dylib it replaces (mkold.py --legacy turns its rebases into classic local relocations).
#   build-gles2x.sh [OUT]     OUT/OpenGLES (default contrib/it-gles/OpenGLES-2x, next to MBXGLEngine)
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
export LEGACY_LINK=1 ARMV6_SDK="${ARMV6_SDK:-$HOME/Developer/ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk}"
. "$HERE/../armv6-toolchain/armv6.sh"
OUT="${1:-$HERE/OpenGLES-2x}"
GEN="$(mktemp -d /tmp/gles2x.XXXXXX)"
trap 'rm -rf "$GEN"' EXIT
python3 "$HERE/genstubs.py" "$HERE/gles_stubs.h" >/dev/null
python3 "$HERE/gles2x_exports.py" gen "$HERE/opengles-2x.exports" \
    "$HERE/../../include/hw/arm/guest-services/gles-names.h" "$GEN"
cc6 "$HERE/gles2x.c" "$GEN/gles2x.o" -x objective-c -I"$GEN" -Wno-objc-root-class
link6 -dylib "$OUT" "$GEN/gles2x.o" -install_name /System/Library/Frameworks/OpenGLES.framework/OpenGLES \
    -compatibility_version 1.0 -current_version 1.0 -exported_symbols_list "$GEN/gles2x.exp" -undefined dynamic_lookup
    
if command -v ldid >/dev/null; then ldid -S "$OUT"; fi
file "$OUT"
