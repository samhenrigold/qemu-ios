#!/bin/bash
# Build KitchenSink.app for iPhone OS 1.0 (README.md here; docs/m68/sideload.md).
#
#   ARMV6_SDK=<iPhoneOS 3.1.3 SDK> ./build.sh ROOT [OUT]
#
# As contrib/hello-2007/build.sh: ROOT is a host copy of the 1.0 root filesystem, whose frameworks are the
# link targets through .tbd stubs; fragile-ABI Objective-C; crt1old entry (LEGACY_LINK=1). art.py draws the
# bundle's images (Pillow).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="${1:?usage: build.sh ROOT [OUT]}"
OUT="${2:-$HERE/build}"
LEGACY_LINK=1
. "$HERE/../armv6-toolchain/armv6.sh"
STUBS="$OUT/stubs"
mkdir -p "$STUBS"
tbd() { python3 "$HERE/../armv6-toolchain/mktbd.py" "$ROOT" "$1" "$STUBS/$2" >/dev/null; }
tbd /usr/lib/libSystem.B.dylib libSystem.tbd
tbd /usr/lib/libobjc.A.dylib libobjc.tbd
for f in UIKit Foundation CoreFoundation CoreGraphics GraphicsServices; do
    tbd /System/Library/Frameworks/$f.framework/$f $f.tbd
done
LEGACY_SYSTEM_STUB="$STUBS"
NAME=KitchenSink
APP="$OUT/Kitchen.app"   # 1.0 labels the icon with the bundle directory name: "KitchenSink" truncates
rm -rf "$APP"
mkdir -p "$APP"
cc6 "$HERE/$NAME.m" "$OUT/$NAME.o" -fobjc-runtime=macosx-fragile-10.5
link6 -execute "$APP/$NAME" "$OUT/$NAME.o" "$STUBS/libobjc.tbd" "$STUBS/UIKit.tbd" "$STUBS/Foundation.tbd" \
    "$STUBS/CoreFoundation.tbd" "$STUBS/CoreGraphics.tbd" "$STUBS/GraphicsServices.tbd"
rm -f "$OUT/$NAME.o"
chmod 755 "$APP/$NAME"
sed "s/@NAME@/$NAME/g; s/@ID@/kitchensink/g" "$HERE/../hello-2007/Info.plist" > "$APP/Info.plist"
printf 'APPL????' > "$APP/PkgInfo"
python3 "$HERE/art.py" "$APP"
echo "$APP"
