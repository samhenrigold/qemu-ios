#!/bin/sh
# iPod touch 2G / 3.1.3 AppSync via the portable dylib, instead of the four
# fixed-offset byte patches in qemu-ios-files/apps/patch-appsync.sh (which is
# kept as the default until this route is proven in regress).
#
# editimg.py script: run against a mounted 3.1.3 rootfs with $MNT set, e.g.
#   editimg.py --nand <copy> --script contrib/appsync/patch-appsync-dylib.sh
# Build the dylib first: contrib/appsync/build.sh (fat armv6+armv7; the armv6
# slice is the one that runs here).
#
# It does the same two things the iPad --appsync path does, both by symbol:
#   1. Patch libmis MISValidateSignature -> success in dyld_shared_cache_armv6,
#      located by symbol (imgtools/appsync_cachepatch.py). This is the iPod's
#      "patch 3", symbol-found rather than at file offset 0x1750EF8.
#   2. Install libappsync.dylib (root-owned) and DYLD_INSERT it into installd,
#      whose interposes carry the installd signer/profile acceptance that the
#      byte patches at installd 0x9F34/0x605C did, plus the SpringBoard launch
#      gate handled by the cache patch (no SpringBoard byte patch needed).
#
# Unlike patch-appsync.sh this makes NO installd/SpringBoard byte edits and needs
# no re-signing: the cache patch is a cache page (covered by amfi_allow_any_signature)
# and the dylib is ldid-signed by build.sh.
set -e
: "${MNT:?run me through editimg.py (it sets MNT)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
DYLIB="${APPSYNC_DYLIB:-$HERE/../../build/appsync/libappsync.dylib}"
CACHEPATCH="$HERE/../../imgtools/appsync_cachepatch.py"
LDID="${LDID:-/opt/homebrew/bin/ldid}"

[ -f "$DYLIB" ] || { echo "missing $DYLIB (run contrib/appsync/build.sh)"; exit 1; }

# 1. shared-cache MISValidateSignature -> success, by symbol.
python3 "$CACHEPATCH" "$MNT/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6" --patch

# 2. install the dylib root-owned and inject it into installd.
install -d "$MNT/usr/lib"
cp "$DYLIB" "$MNT/usr/lib/libappsync.dylib"
chmod 644 "$MNT/usr/lib/libappsync.dylib"

python3 - "$MNT/System/Library/LaunchDaemons/com.apple.mobile.installd.plist" <<'PY'
import sys, plistlib
p = sys.argv[1]
with open(p, "rb") as f:
    data = f.read()
d = plistlib.loads(data)
env = d.setdefault("EnvironmentVariables", {})
libs = [x for x in env.get("DYLD_INSERT_LIBRARIES", "").split(":") if x]
if "/usr/lib/libappsync.dylib" not in libs:
    libs.append("/usr/lib/libappsync.dylib")
env["DYLD_INSERT_LIBRARIES"] = ":".join(libs)
fmt = plistlib.FMT_BINARY if data[:6] == b"bplist" else plistlib.FMT_XML
with open(p, "wb") as f:
    f.write(plistlib.dumps(d, fmt=fmt))
print("installd DYLD_INSERT_LIBRARIES =", env["DYLD_INSERT_LIBRARIES"])
PY

# editimg.py runs setowner afterwards for files it knows; make ownership explicit
# here too in case this script is run standalone.
echo "appsync dylib installed; remember: file must be uid 0 (editimg/setowner handles it)"
