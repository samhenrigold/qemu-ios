#!/bin/sh
# AppSync for iPod touch 2G firmware: cache patch where present, plus process-local
# installation hooks. 2.x uses the Lockbot argument launcher; 3.0+ uses installd.
# Standalone libmis remains stock. Run via editimg.py with MNT set.
# Build both helpers first with contrib/appsync/build.sh.
set -e
: "${MNT:?run me through editimg.py (it sets MNT)}"
HERE="$(cd "$(dirname "$0")" && pwd)"
DYLIB="${APPSYNC_DYLIB:-$HERE/../../build/appsync/libappsync.dylib}"
CACHEPATCH="$HERE/../../imgtools/appsync_cachepatch.py"
LDID="${LDID:-/opt/homebrew/bin/ldid}"

[ -f "$DYLIB" ] || { echo "missing $DYLIB (run contrib/appsync/build.sh)"; exit 1; }

# 0. Optional: undo the old byte-patched AppSync. Images made by
#    qemu-ios-files/apps/patch-appsync.sh carry an edited, re-signed installd and
#    SpringBoard; STOCK_ROOT (a mounted stock rootfs of the same build) puts the
#    Apple-signed originals back, so only the dylib + cache patch remain.
if [ -n "${STOCK_ROOT:-}" ]; then
    for f in usr/libexec/installd System/Library/CoreServices/SpringBoard.app/SpringBoard; do
        cp "$STOCK_ROOT/$f" "$MNT/$f"
        chmod 755 "$MNT/$f"
        echo "restored stock /$f"
    done
fi

# 1. shared-cache MISValidateSignature -> success, by symbol.
CACHE="$MNT/System/Library/Caches/com.apple.dyld/dyld_shared_cache_armv6"
if [ -f "$CACHE" ]; then
    python3 "$CACHEPATCH" "$CACHE" --patch
fi

# 2. install the dylib root-owned and inject it into installd.
install -d "$MNT/usr/lib"
cp "$DYLIB" "$MNT/usr/lib/libappsync.dylib"
chmod 644 "$MNT/usr/lib/libappsync.dylib"

# installd's launchd job: com.apple.mobile.installd.plist on 3.2 (iPad),
# com.apple.installd.plist on 3.1.3 (iPod). Inject into whichever exists.
python3 - "$MNT/System/Library/LaunchDaemons" "$DYLIB" <<'PY'
import sys, os, plistlib, shutil
ld = sys.argv[1]
cands = ["com.apple.mobile.installd.plist", "com.apple.installd.plist"]
p = next((os.path.join(ld, c) for c in cands if os.path.exists(os.path.join(ld, c))), None)
service = p is None
if service:
    p = os.path.join(os.path.dirname(ld), "Lockdown", "Services.plist")
with open(p, "rb") as f:
    data = f.read()
root = plistlib.loads(data)
d = root
if service:
    d = root.get("com.apple.mobile.installation_proxy", {})
    if d.get("ProgramArguments", [None])[0] != "/usr/libexec/mobile_installation_proxy":
        sys.exit("no supported installation service")
if service:
    launcher = os.path.join(os.path.dirname(sys.argv[2]), "appsync-launch")
    dest = os.path.join(os.environ["MNT"], "usr/libexec/appsync-launch")
    shutil.copyfile(launcher, dest)
    os.chmod(dest, 0o755)
    d["ProgramArguments"] = ["/usr/libexec/appsync-launch"] + d["ProgramArguments"]
else:
    env = d.setdefault("EnvironmentVariables", {})
    libs = [x for x in env.get("DYLD_INSERT_LIBRARIES", "").split(":") if x]
    if "/usr/lib/libappsync.dylib" not in libs:
        libs.append("/usr/lib/libappsync.dylib")
    env["DYLD_INSERT_LIBRARIES"] = ":".join(libs)
fmt = plistlib.FMT_BINARY if data[:6] == b"bplist" else plistlib.FMT_XML
with open(p, "wb") as f:
    f.write(plistlib.dumps(root, fmt=fmt))
print("AppSync installed for installation service (%s)" % os.path.basename(p))
PY

# editimg.py runs setowner afterwards for files it knows; make ownership explicit
# here too in case this script is run standalone.
echo "appsync dylib installed; remember: file must be uid 0 (editimg/setowner handles it)"
