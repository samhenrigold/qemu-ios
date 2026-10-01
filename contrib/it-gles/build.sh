#!/bin/bash
# Build the guest-side GLES binaries. See ../armv6-toolchain/README.md.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
# The optional legacy fixture is isolated; the default renderer/helper build below is unchanged.
if [ "${1:-}" = --flavor ] && [ "${2:-}" = ios2 ]; then
    shift 2
    OUT="$HERE/build-ios2"
    if [ "${1:-}" = --out ] && [ $# -eq 2 ]; then OUT="$2"; shift 2; fi
    [ $# -eq 0 ] || { echo "usage: build.sh [--flavor ios2 [--out DIR]]" >&2; exit 1; }
    : "${ARMV6_SDK:?ios2 requires an explicitly supplied SDK 2.0 path}"
    GUEST_API_VERSION=2.0; LEGACY_LINK=1; GUEST_ARCH=armv6
    . "$HERE/../armv6-toolchain/armv6.sh"
    command -v ldid >/dev/null || { echo "ldid required for ios2 fixture" >&2; exit 1; }
    mkdir -p "$OUT/GLTest.app"
    LINK_STUB_DIR="$(mktemp -d "$OUT/link-stub.XXXXXX")"
    trap 'rm -rf "$LINK_STUB_DIR"' EXIT
    python3 "$HERE/../it-harness/audit.py" unused --sdk "$ARMV6_SDK" --sdk-version 2.0 --copy-link-stub "$LINK_STUB_DIR/libSystem.dylib"
    LEGACY_SYSTEM_STUB="$LINK_STUB_DIR"
    cc6 "$HERE/glapp.c" "$OUT/glapp.o" -idirafter "$(xcrun clang -print-resource-dir)/include"
    link6 -execute "$OUT/GLTest.app/GLTest" "$OUT/glapp.o"
    chmod 755 "$OUT/GLTest.app/GLTest"
    ldid -S "$OUT/GLTest.app/GLTest"
    python3 - "$HERE/glapp-Info.plist" "$OUT" <<'PYBUILD'
import plistlib,sys,zipfile
from pathlib import Path
info=plistlib.loads(Path(sys.argv[1]).read_bytes());info['MinimumOSVersion']='2.0';info['DTSDKName']='iphoneos2.0'
out=Path(sys.argv[2]);plist=(out/'GLTest.app/Info.plist');plist.write_bytes(plistlib.dumps(info))
with zipfile.ZipFile(out/'GLTest.ipa','w',zipfile.ZIP_DEFLATED) as archive:
    for path in (out/'GLTest.app').iterdir(): archive.write(path,'Payload/GLTest.app/'+path.name)
PYBUILD
    python3 "$HERE/../it-harness/audit.py" "$OUT/GLTest.app/GLTest" --sdk "$ARMV6_SDK" --legacy > "$OUT/binary-audit.json"
    echo "Legacy GLES fixture: $OUT/GLTest.ipa (native qualification still required)"
    exit 0
fi
[ $# -eq 0 ] || { echo "usage: build.sh [--flavor ios2 [--out DIR]]" >&2; exit 1; }
. "$HERE/../armv6-toolchain/armv6.sh"

# The name table and the dispatch stubs the core (mbxshim.c, compiled into contrib/gles-public and gles1x.c) uses.
python3 "$HERE/../gles-public/gligen.py" --check
python3 "$HERE/genstubs.py" "$HERE/gles_stubs.h" >/dev/null

# 1.x (the iPod touch 1G): OpenGLES itself is the driver, with no EAGL and the old ObjC runtime, so the same core
# goes in as the framework binary under 1.x's own export names (build-gles1x.sh, gles1x.c). 2.x-5.x take the
# one front end, contrib/gles-public.
bash "$HERE/build-gles1x.sh" "$HERE/OpenGLES-1x"

# GLTest.app -- a real app bundle with a CAEAGLLayer. UIKit, QuartzCore, Foundation,
# OpenGLES and libobjc are all dlopen'd, so nothing here links anything but libSystem:
# `ld -framework OpenGLES` against the 3.1.3 SDK is a hard error (its 2009 dylibs read
# as platform 'unknown', fatal for a framework though only a warning for -l).
cc6 "$HERE/glapp.c" "$HERE/glapp.o"
link6 -execute "$HERE/GLTest" "$HERE/glapp.o"
rm -f "$HERE/glapp.o"

cc6 "$HERE/sblaunch.c" "$HERE/sblaunch.o"
link6 -execute "$HERE/sblaunch" "$HERE/sblaunch.o"
rm -f "$HERE/sblaunch.o"
# SpringBoard rejects SBSLaunchApplicationWithIdentifier from a caller without
# com.apple.springboard.launchapplications, and reports the refusal only to the
# device console -- the call itself returns 1, exactly as it does for an app
# that is not installed. Signing with the entitlement is not optional.
if command -v ldid >/dev/null; then
    ldid "-S$HERE/sblaunch-entitlements.xml" "$HERE/sblaunch"
fi

APP="$HERE/GLTest.app"
rm -rf "$APP"
mkdir -p "$APP"
cp "$HERE/GLTest" "$APP/GLTest"
cp "$HERE/glapp-Info.plist" "$APP/Info.plist"
chmod 755 "$APP/GLTest"
# Ad-hoc sign it. The image's boot args disable enforcement, but installd and
# SpringBoard both walk the signature before anything is executed, and an
# unsigned binary is a different rejection from a bad one.
if command -v ldid >/dev/null; then
    ldid -S "$APP/GLTest"
fi

file "$HERE/OpenGLES-1x" "$APP/GLTest"
