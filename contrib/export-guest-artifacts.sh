#!/bin/bash
# Build and stage everything LightTouchMac takes from this checkout, with a manifest.
#
#   contrib/export-guest-artifacts.sh OUT [QEMU-BUILD-DIR]
#
# OUT (a new directory) gets:
#   guest-tools/        the iPod set the app uploads through the guest agent (11 payloads)
#   ipad-guest-tools/   the flat directory firmwarekit reads (--guest-tools): the iPad helpers, AppSync, the GL
#                       front end (OpenGLES, fat armv6 + armv7, contrib/gles-public: every k48 and n72 build) with
#                       gles-names.h, the name table it and the host speak, the n72 recipe's inputs, the n45 recipe's
#                       (OpenGLES-1x, 1.x's own front end, and opengles-1x.exports, the export set it must match),
#                       the armv6 it_keybag and it_prefs, and armv6.itpack /
#                       armv7.itpack (contrib/guest-package, VERSION's serial)
#   macos-app/entitlements.plist   the app helper's entitlements
#   include/ios-app/, include/macos-app/   the headers the helper compiles against
#   dylib/libqemu-arm.dylib        only with QEMU-BUILD-DIR: make-dylib-macos.sh on that configured build
#   manifest.json       source commit/branch/dirty, the guest-package serial and version, sha256 of every input
#                       (the contrib sources and the name table the build read) and of every staged file
#   build/              the guest-package build tree (its own copy of the sources; logs under build/logs)
# Environment, as the contrib recipes take it: ARMV6_SDK (iPhoneOS3.1.3.sdk), IPAD_SDK (iPhoneOS3.2.sdk), LDID.
# Every payload is built by its component's own build.sh from the copy under build/src (guest-package/build.sh),
# so the checkout is never written and a stale tracked binary is never shipped: a payload that did not build
# is an error. A recipe that fails after its shipped payloads (a probe) is a warning, as for the packages.
set -euo pipefail
fail() { echo "export-guest-artifacts: $*" >&2; exit 1; }
[ "$#" -ge 1 ] && [ "$#" -le 2 ] || fail "usage: $0 OUT [QEMU-BUILD-DIR]"
SRC="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$1"
QEMU_BUILD="${2:-}"
[ ! -e "$OUT" ] && [ ! -L "$OUT" ] || fail "use a new output directory: $OUT"
export ARMV6_SDK="${ARMV6_SDK:-$HOME/Developer/ipod2g-re/OldSDK/iPhoneOS3.1.3.sdk}"
export IPAD_SDK="${IPAD_SDK:-$HOME/Developer/qemu-ios-files/ipad1/sdk/x-iPhoneSDK3_2_2/Payload/Platforms/iPhoneOS.platform/Developer/SDKs/iPhoneOS3.2.sdk}"
export IPOD_SDK="$ARMV6_SDK"
for sdk in ARMV6_SDK IPAD_SDK; do
    [ -f "${!sdk}/usr/lib/libSystem.dylib" ] || fail "no SDK at $sdk=${!sdk}"
done
for tool in python3 xcrun file lipo; do
    command -v "$tool" >/dev/null || fail "required tool not found: $tool"
done
LDID="$(command -v "${LDID:-ldid}")" || fail "guest signer not found: ${LDID:-ldid}"
export LDID
if [ -n "$QEMU_BUILD" ]; then
    [ -f "$QEMU_BUILD/build.ninja" ] || fail "not a configured QEMU build: $QEMU_BUILD"
    QEMU_BUILD="$(cd "$QEMU_BUILD" && pwd)"
fi

mkdir -p "$(dirname "$OUT")"
mkdir "$OUT"
OUT="$(cd "$OUT" && pwd)"
B="$OUT/build"

echo "building the guest components and packages (contrib/guest-package/build.sh)"
if ! bash "$SRC/contrib/guest-package/build.sh" "$B" >"$OUT/build.log" 2>&1; then
    tail -40 "$OUT/build.log" >&2
    fail "guest-package build failed; see $OUT/build.log"
fi
grep 'guest-package: warning' "$OUT/build.log" >&2 || true
# The iPod's armv6 it_keybag and it_prefs: recipes that write build/ipod-guest next to their (copied) sources.
for recipe in it-keybag it-prefs; do
    if ! bash "$B/src/contrib/$recipe/build-ipod.sh" >"$B/logs/$recipe-ipod.log" 2>&1; then
        cat "$B/logs/$recipe-ipod.log" >&2
        fail "$recipe/build-ipod.sh failed"
    fi
done

# Stage only after every build has succeeded. Each Mach-O keeps its ldid signature (it runs in the guest); the
# app's signature seals them as resources.
C="$B/src/contrib"; G="$B/src/build"; NAMES="$B/src/include/hw/arm/guest-services/gles-names.h"
stage() {   # DIR SOURCE [NAME]
    [ -s "$2" ] || fail "build did not produce required payload: $2"
    cp -p "$2" "$OUT/$1.incomplete/${3:-$(basename "$2")}"
}
mkdir "$OUT/guest-tools.incomplete" "$OUT/ipad-guest-tools.incomplete"
for p in it-instprogress/sbdlicon it-halt/ithalt it-agent/it_agent it-agent/it_typein.dylib \
         it-agent/com.qemu.it-agent.plist it-status/itstatus it-media/itmedia it-media/itphoto it-proxy/itproxy \
         it-proxy/ittrust it-orientation/itorient; do
    stage guest-tools "$C/$p"
done
# The iPad set, by the file names firmwarekit reads (SystemEdits.Helpers, Preparer's it_keybag).
for t in it_pbd it_ethlink it_prefs it_msmquiet.dylib it_seal it_keybag it_gltest; do
    stage ipad-guest-tools "$G/ipad1-guest/$t"
done
stage ipad-guest-tools "$G/appsync/libappsync.dylib"
stage ipad-guest-tools "$G/appsync/appsync-launch"
for j in it-pasteboard/com.qemu.it-pbd.plist it-ethlink/com.qemu.it-ethlink.plist it-prefs/com.qemu.it-prefs.plist \
         it-seal/com.qemu.it-seal.plist it-gltest/com.qemu.it-gltest.plist; do
    stage ipad-guest-tools "$C/$j"
done
# The GL front end (one fat OpenGLES.framework/OpenGLES for every iPad build), plus the name table (the wire ids)
# it and the host were built from.
stage ipad-guest-tools "$C/gles-public/OpenGLES"
stage ipad-guest-tools "$NAMES"
stage ipad-guest-tools "$B/armv6.itpack"
stage ipad-guest-tools "$B/armv7.itpack"
# The n72 recipe's inputs (new iPods from a stock IPSW); libappsync.dylib is the fat one above.
for p in it-gles/sblaunch it-instprogress/sbdlicon it-agent/it_agent it-agent/it_typein.dylib \
         it-agent/com.qemu.it-agent.plist; do
    stage ipad-guest-tools "$C/$p"
done
# 1.x: its own GL front end that replaces OpenGLES.framework/OpenGLES (gles2x.c without EAGL), and
# the export set the preparer checks the stock binary against before the package's hook may replace it.
stage ipad-guest-tools "$C/it-gles/OpenGLES-1x"
stage ipad-guest-tools "$C/it-gles/opengles-1x.exports"
stage ipad-guest-tools "$G/ipod-guest/it_keybag" it_keybag-armv6
stage ipad-guest-tools "$G/ipod-guest/it_prefs" it_prefs-armv6
chmod 0644 "$OUT"/ipad-guest-tools.incomplete/*.plist "$OUT"/ipad-guest-tools.incomplete/*.h \
    "$OUT"/ipad-guest-tools.incomplete/*.exports

mkdir -p "$OUT/macos-app" "$OUT/include/ios-app" "$OUT/include/macos-app"
cp -p "$SRC/contrib/macos-app/entitlements.plist" "$OUT/macos-app/"
cp -p "$SRC"/contrib/ios-app/*.h "$OUT/include/ios-app/"
cp -p "$SRC"/contrib/macos-app/*.h "$OUT/include/macos-app/"
if [ -n "$QEMU_BUILD" ]; then
    echo "linking libqemu-arm.dylib (contrib/macos-app/make-dylib-macos.sh $QEMU_BUILD)"
    bash "$SRC/contrib/macos-app/make-dylib-macos.sh" "$QEMU_BUILD" >"$OUT/dylib.log" 2>&1 || { tail -20 "$OUT/dylib.log" >&2; fail "dylib failed; see $OUT/dylib.log"; }
    mkdir "$OUT/dylib"
    cp -p "$QEMU_BUILD/libqemu-arm.dylib" "$OUT/dylib/"
fi

python3 - "$SRC" "$OUT" "$QEMU_BUILD" <<'PY'
import hashlib, json, os, subprocess, sys
from pathlib import Path

src, out = Path(sys.argv[1]), Path(sys.argv[2])
qemu_build = sys.argv[3] or None
sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
def git(*args):
    try:
        return subprocess.check_output(["git", "-C", str(src), *args], text=True, stderr=subprocess.DEVNULL).strip()
    except (subprocess.CalledProcessError, OSError):
        return None
commit = git("rev-parse", "HEAD")
# The inputs: what guest-package/build.sh copies (sources only: generated headers and binaries are not inputs).
components = ("armv6-toolchain it-gles gles-public it-agent it-instprogress it-media it-proxy it-status it-halt it-orientation "
              "ipad1-guest appsync it-boot it-pasteboard it-ethlink it-seal it-prefs it-keybag it-heading "
              "it-cctest it-gltest it-msmquiet guest-package").split()
inputs = {}
for c in components:
    for f in sorted((src / "contrib" / c).iterdir()):
        if f.is_file() and f.suffix in (".c", ".h", ".sh", ".py", ".xml", ".plist", ".entitlements", ".txt",
                                        ".exports") \
                and f.name not in ("gles_stubs.h",):
            inputs[str(f.relative_to(src))] = sha(f)
for f in sorted([src / "include/hw/arm/guest-services/gles-names.h",
                 src / "contrib/guest-package/VERSION", src / "contrib/export-guest-artifacts.sh"]):
    inputs[str(f.relative_to(src))] = sha(f)
files = {}
for d in ("guest-tools.incomplete", "ipad-guest-tools.incomplete", "macos-app", "include", "dylib"):
    for f in sorted((out / d).rglob("*")):
        if f.is_file():
            files[str(f.relative_to(out)).replace(".incomplete/", "/", 1)] = sha(f)
version = dict(l.split(None, 1) for l in (src / "contrib/guest-package/VERSION").read_text().splitlines() if l.strip())
warnings = [l.strip() for l in (out / "build.log").read_text(errors="replace").splitlines() if "guest-package: warning" in l]
manifest = {
    "schema": 1,
    "source": {"path": str(src), "commit": commit, "branch": git("rev-parse", "--abbrev-ref", "HEAD"),
               "dirty": bool(git("status", "--porcelain")) if commit else True},
    "guest_package": {"serial": int(version["serial"]), "version": version["version"].strip()},
    "sdk": {"armv6": os.environ["ARMV6_SDK"], "ipad": os.environ["IPAD_SDK"]},
    "signer": os.environ["LDID"],
    "qemu_build": qemu_build,
    "warnings": warnings,
    "inputs": inputs,
    "files": files,
}
(out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
PY
mv "$OUT/ipad-guest-tools.incomplete" "$OUT/ipad-guest-tools"
mv "$OUT/guest-tools.incomplete" "$OUT/guest-tools"
echo "exported to $OUT ($(python3 -c 'import json,sys; m=json.load(open(sys.argv[1])); print(len(m["files"]), "files, source", (m["source"]["commit"] or "no git")[:10], "dirty" if m["source"]["dirty"] else "clean")' "$OUT/manifest.json"))"
