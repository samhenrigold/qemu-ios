#!/bin/bash
# Build the iPhone OS 1.x OpenGLES framework; 2.x–5.x use gles-public/build.sh.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
export LEGACY_LINK=1 ARMV6_SDK="${ARMV6_SDK:-$HOME/Developer/ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk}"
. "$HERE/../armv6-toolchain/armv6.sh"
OUT="${1:-$HERE/OpenGLES-1x}"
GEN="$(mktemp -d /tmp/gles1x.XXXXXX)"
trap 'rm -rf "$GEN"' EXIT
sh "$HERE/genstubs.sh" "$HERE/gles_stubs.h"
sh "$HERE/gles-exports.sh" "$HERE/opengles-1x.exports" \
    "$HERE/../../include/hw/arm/guest-services/gles-names.h" "$GEN"
cc6 "$HERE/gles1x.c" "$GEN/gles1x.o" -I"$GEN"
link6 -dylib "$OUT" "$GEN/gles1x.o" -install_name /System/Library/Frameworks/OpenGLES.framework/OpenGLES \
    -compatibility_version 1.0 -current_version 1.0 -exported_symbols_list "$GEN/gles2x.exp"
# 1.x predates code signing; preserve the stock unsigned framework format.
file "$OUT"
