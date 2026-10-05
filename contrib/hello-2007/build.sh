#!/bin/bash
# Build Hello.app for iPhone OS 1.0 (docs/m68/sideload.md).
#
#   ARMV6_SDK=<iPhoneOS 3.1.3 SDK> ./build.sh ROOT [OUT]
#
# ROOT is a host copy of the 1.0 root filesystem (the decrypted IPSW rootfs, mounted or copied): its own
# UIKit, Foundation, CoreFoundation, CoreGraphics, GraphicsServices, libobjc and libSystem are the link
# targets, through .tbd stubs (../armv6-toolchain/mktbd.py). Nothing from them is copied into the output.
# Objective-C is compiled for the fragile ABI (the __OBJC segment 1.x's runtime reads); the executable
# enters at crt1old.c's _start (LEGACY_LINK=1), as 1.x libSystem needs.
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${1:?usage: build.sh ROOT [OUT]}"
OUT="${2:-$HERE/build}"
LEGACY_LINK=1
. "$HERE/../armv6-toolchain/armv6.sh"
APP="$OUT/Hello.app"
STUBS="$OUT/stubs"
mkdir -p "$APP" "$STUBS"
tbd() { python3 "$HERE/../armv6-toolchain/mktbd.py" "$ROOT" "$1" "$STUBS/$2" >/dev/null; }
tbd /usr/lib/libSystem.B.dylib libSystem.tbd
tbd /usr/lib/libobjc.A.dylib libobjc.tbd
for f in UIKit Foundation CoreFoundation CoreGraphics GraphicsServices; do
    tbd /System/Library/Frameworks/$f.framework/$f $f.tbd
done
LEGACY_SYSTEM_STUB="$STUBS"
cc6 "$HERE/Hello.m" "$OUT/Hello.o" -fobjc-runtime=macosx-fragile-10.5
link6 -execute "$APP/Hello" "$OUT/Hello.o" "$STUBS/libobjc.tbd" "$STUBS/UIKit.tbd" "$STUBS/Foundation.tbd" \
    "$STUBS/CoreFoundation.tbd" "$STUBS/CoreGraphics.tbd" "$STUBS/GraphicsServices.tbd"
rm -f "$OUT/Hello.o"
chmod 755 "$APP/Hello"
cp "$HERE/Info.plist" "$APP/Info.plist"
printf 'APPL????' > "$APP/PkgInfo"
python3 "$HERE/icon.py" "$APP/icon.png"
echo "$APP"
