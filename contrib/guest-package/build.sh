#!/bin/bash
# Build every guest package and pack one .itpack per arch:
#   OUT/packages/<family>/  n45-ios1, n72-ios2/30/3, k48-ios30/3/4/5/6/7 (n72-ios4: stub)
#   OUT/armv6.itpack        n72-* packages + the legacy-linked loader
#   OUT/armv7.itpack        k48-* packages + the loader (+ the legacy-linked one for k48-ios30)
# The components' own build.sh recipes run on a copy of their sources under
# OUT/src, so the checkout's tracked binaries stay untouched. Bump VERSION's
# serial for every release: it_boot compares serials, never versions.
#   build.sh [OUT]          default build/guest-package (untracked)
# ARMV6_SDK (3.1.3) and IPAD_SDK (3.2) default as the recipes do; LDID too.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
SRC="$(cd "$HERE/../.." && pwd)"
OUT="${1:-$SRC/build/guest-package}"
export ARMV6_SDK="${ARMV6_SDK:-$HOME/Developer/ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk}"
export IPOD_SDK="${IPOD_SDK:-$ARMV6_SDK}"
[ -f "$ARMV6_SDK/usr/lib/libSystem.dylib" ] || { echo "ARMV6_SDK: no 3.1.3 SDK at $ARMV6_SDK" >&2; exit 1; }
rm -rf "$OUT/src" "$OUT/logs"
mkdir -p "$OUT/src/contrib" "$OUT/src/include/hw/arm/guest-services" "$OUT/src/tests" "$OUT/logs"
COMPONENTS="it-gles gles-public it-agent it-instprogress it-media it-proxy it-status it-halt it-orientation
            ipad1-guest appsync it-boot"
# ipad1-guest also compiles these sources; it-gles/gles-public/it-boot read their neighbors
for c in $(bash "$HERE/build-inputs.sh" --components); do
    cp -R "$SRC/contrib/$c" "$OUT/src/contrib/"
done
# the GL shims and the host share the name table (the wire ids)
cp "$SRC/include/hw/arm/guest-services/gles-names.h" "$OUT/src/include/hw/arm/guest-services/"
cp -R "$SRC/tests/guest-package" "$OUT/src/tests/"
# binaries checked in or left by earlier builds are not inputs: only what builds here ships
find "$OUT/src" -type f -print0 | while IFS= read -r -d '' f; do
    case "$(od -An -tx1 -N4 "$f" | tr -d ' \n')" in cefaedfe|cafebabe|cffaedfe) rm -f "$f" ;; esac
done
# A recipe that fails after its shipped payloads (a broken probe) only warns:
# mkpkg refuses any payload that did not build.
for c in $COMPONENTS; do
    echo "building $c"
    # One armv6 helper ABI for 2.x through 4.x. These recipes only build
    # armv6; ipad1-guest builds its armv7 helpers independently. The older
    # linker mode also reserves r9, which 2.x uses as its thread pointer.
    legacy=0
    case "$c" in it-agent|it-instprogress|it-gles) legacy=1 ;; esac
    if ! LEGACY_LINK="$legacy" bash "$OUT/src/contrib/$c/build.sh" >"$OUT/logs/$c.log" 2>&1; then
        echo "guest-package: warning: $c/build.sh failed (log: $OUT/logs/$c.log)" >&2
    fi
done
# armv7 on 3.0 (k48-ios30): the iPad helpers again, legacy-linked against the 3.1.3 SDK for 3.0's dyld.
if ! LEGACY_LINK=1 IPAD_SDK="$ARMV6_SDK" bash "$OUT/src/contrib/ipad1-guest/build.sh" "$OUT/src/build/ipad1-guest-legacy" \
        >"$OUT/logs/ipad1-guest-legacy.log" 2>&1; then
    echo "guest-package: warning: ipad1-guest/build.sh (legacy) failed (log: $OUT/logs/ipad1-guest-legacy.log)" >&2
fi
# The iPod's armv6 it_prefs (every n72 package): its own recipe, legacy-linked for 2.x's dyld.
if ! LEGACY_LINK=1 bash "$OUT/src/contrib/it-prefs/build-ipod.sh" >"$OUT/logs/it-prefs-ipod.log" 2>&1; then
    echo "guest-package: warning: it-prefs/build-ipod.sh failed (log: $OUT/logs/it-prefs-ipod.log)" >&2
fi
# mkpkg (mkpkg.c), a host tool built here for this build
mkdir -p "$OUT/tools"
xcrun clang -O2 -Wall -Werror -o "$OUT/tools/mkpkg" "$HERE/mkpkg.c" -lz -framework CoreFoundation
"$OUT/tools/mkpkg" selfcheck "$OUT/src"
"$OUT/tools/mkpkg" build "$OUT/src" "$OUT"
