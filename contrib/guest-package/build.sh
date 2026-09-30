#!/bin/bash
# Build every guest package and pack one .itpack per arch:
#   OUT/packages/<family>/  n72-ios3, k48-ios3, k48-ios4, k48-ios5 (n72-ios2, n72-ios4: stubs)
#   OUT/armv6.itpack        n72-* packages + the legacy-linked loader
#   OUT/armv7.itpack        k48-* packages + the loader
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
# ipad1-guest also compiles these sources; it-gles/gles-public/it-boot read their neighbours
for c in armv6-toolchain $COMPONENTS it-pasteboard it-ethlink it-seal it-prefs it-keybag it-heading \
         it-cctest it-gltest it-msmquiet guest-package; do   # guest-package: it-boot's test imports mkpkg
    cp -R "$SRC/contrib/$c" "$OUT/src/contrib/"
done
# the GL shims and the host share the name table (the wire ids)
cp "$SRC/include/hw/arm/guest-services/gles-names.h" "$OUT/src/include/hw/arm/guest-services/"
cp -R "$SRC/tests/guest-package" "$OUT/src/tests/"
# binaries checked in or left by earlier builds are not inputs: only what builds here ships
python3 - "$OUT/src" <<'PY'
import os, sys
for d, _, names in os.walk(sys.argv[1]):
    for n in names:
        p = os.path.join(d, n)
        with open(p, "rb") as f:
            if f.read(4) in (b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xcf\xfa\xed\xfe"):
                os.unlink(p)
PY
# A recipe that fails after its shipped payloads (a broken probe) only warns:
# mkpkg.py refuses any payload that did not build.
for c in $COMPONENTS; do
    echo "building $c"
    if ! bash "$OUT/src/contrib/$c/build.sh" >"$OUT/logs/$c.log" 2>&1; then
        echo "guest-package: warning: $c/build.sh failed (log: $OUT/logs/$c.log)" >&2
    fi
done
python3 "$HERE/mkpkg.py" build "$OUT/src" "$OUT"
