#!/bin/bash
#
# Build "iPod touch.app" -- a self-contained macOS app that boots the emulated
# device with no terminal involved.
#
#     contrib/macos-app/build-app.sh [--images <dir>] [--out <dir>]
#         [--sign "Developer ID Application: ..."] [--notarize <keychain profile>]
#
# --images defaults to ~/Developer/qemu-ios-files and must contain the release
# assets, unpacked: bootrom_240_4, ios3/nor_7E18.bin, ios3/iBoot.bin and a NAND
# page directory (nand-appsync3 by default).
#
# The result is one bundle with the emulator, every dylib it links, the images
# and the helper scripts inside it. It writes nothing to the bundle at runtime:
# guest writes go to a copy-on-write NAND overlay under Application Support, so
# the app stays exactly as shipped and deleting that folder is a factory reset.
#
# With no --sign the bundle is ad-hoc signed: enough to run on the machine that
# built it, not enough for Gatekeeper anywhere else ("cannot be opened because
# the developer cannot be verified", and a right-click ▸ Open to get past it).
#
# --sign with a Developer ID and --notarize with a stored notarytool profile
# produce something that opens by double-click on any Mac. Store the profile
# once with:
#
#     xcrun notarytool store-credentials <profile> \
#         --key <AuthKey_XXXX.p8> --key-id <KEY ID> --issuer <ISSUER UUID>
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/../.." && pwd)"
IMAGES="${IPOD_FILES:-$HOME/Developer/qemu-ios-files}"
OUT="$TREE/build"
NAND_NAME="nand-appsync3"
QEMU="$TREE/build/qemu-system-arm"
SIGN_ID="${SIGN_ID:-}"
NOTARIZE_PROFILE="${NOTARIZE_PROFILE:-}"

while [ $# -gt 0 ]; do
    case "$1" in
    --images) IMAGES="$2"; shift ;;
    --out)    OUT="$2"; shift ;;
    --nand)   NAND_NAME="$2"; shift ;;
    --qemu)   QEMU="$2"; shift ;;
    --sign)   SIGN_ID="$2"; shift ;;
    --notarize) NOTARIZE_PROFILE="$2"; shift ;;
    -h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "build-app.sh: unknown flag $1" >&2; exit 2 ;;
    esac
    shift
done

APP="$OUT/iPod touch.app"
C="$APP/Contents"

die() { echo "build-app.sh: $*" >&2; exit 1; }

[ -x "$QEMU" ] || die "no emulator at $QEMU -- build it first (ninja -C build qemu-system-arm)"
for f in "bootrom_240_4" "ios3/nor_7E18.bin" "ios3/iBoot.bin" "$NAND_NAME/cs0"; do
    [ -e "$IMAGES/$f" ] || die "missing $IMAGES/$f -- unpack the release into --images"
done

echo "==> assembling $APP"
rm -rf "$APP"
mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources/tools"

cp "$QEMU" "$C/MacOS/qemu-system-arm"

# ---------------------------------------------------------------- the dylibs
#
# Every non-system dylib the binary links, and everything THOSE link, copied in
# and repointed at the bundle. Homebrew paths are absolute, so without this the
# app runs on this machine and nowhere else -- and the failure on someone else's
# Mac is a dyld abort before any of our code gets to say anything.
bundle_libs() {
    local target="$1" lib rel
    local -a queue=("$target")

    while [ ${#queue[@]} -gt 0 ]; do
        local cur="${queue[0]}"
        queue=("${queue[@]:1}")

        while read -r lib; do
            case "$lib" in
                /usr/lib/*|/System/*|@*|"") continue ;;
            esac
            local base; base="$(basename "$lib")"
            if [ ! -e "$C/Frameworks/$base" ]; then
                cp "$lib" "$C/Frameworks/$base"
                chmod u+w "$C/Frameworks/$base"
                install_name_tool -id "@rpath/$base" "$C/Frameworks/$base" 2>/dev/null || true
                queue+=("$C/Frameworks/$base")
            fi
            install_name_tool -change "$lib" "@rpath/$base" "$cur" 2>/dev/null || true
        done < <(otool -L "$cur" | tail -n +2 | awk '{print $1}')
    done
}

echo "==> bundling libraries"
bundle_libs "$C/MacOS/qemu-system-arm"
install_name_tool -add_rpath "@executable_path/../Frameworks" \
    "$C/MacOS/qemu-system-arm" 2>/dev/null || true
for f in "$C/Frameworks/"*.dylib; do
    install_name_tool -add_rpath "@loader_path" "$f" 2>/dev/null || true
done
echo "    $(ls "$C/Frameworks" | wc -l | tr -d ' ') libraries"

# usbmuxd is what makes app install and the guest shell work; the emulator dials
# out to it. It is a separate fork, so bundle it when it is there and let the
# app run without it when it is not -- everything except USB still works.
MUXD="${MUXD:-$HOME/Developer/usbmuxd-qemu/usbmuxd/src/usbmuxd}"
if [ -x "$MUXD" ]; then
    cp "$MUXD" "$C/Resources/tools/usbmuxd"
    bundle_libs "$C/Resources/tools/usbmuxd"
    install_name_tool -add_rpath "@executable_path/../../Frameworks" \
        "$C/Resources/tools/usbmuxd" 2>/dev/null || true
    echo "==> bundled usbmuxd"
else
    echo "==> no usbmuxd at $MUXD -- Install App and Open Terminal will be unavailable" >&2
fi

# ---------------------------------------------------------------- the payload
#
# The NAND goes in as ONE opaque file, not as pages and not as a tarball. See
# contrib/macos-app/nandpack.py for the full reason; the short version is that
# Apple's notary service walks every file in the app looking for Mach-O
# binaries, a NAND page often starts with an armv6 Mach-O header, and it
# DECOMPRESSES ARCHIVES to keep walking -- so .tar.gz was rejected for the same
# pages one layer deeper.
#
# The bootrom, NOR and iBoot stay as ordinary files: they are not Mach-O and the
# scanner has never objected to them.
echo "==> packing images (this is the slow part)"
mkdir -p "$C/Resources/device/ios3"
cp "$IMAGES/bootrom_240_4"     "$C/Resources/device/"
cp "$IMAGES/ios3/nor_7E18.bin" "$C/Resources/device/ios3/"
cp "$IMAGES/ios3/iBoot.bin"    "$C/Resources/device/ios3/"
python3 "$HERE/nandpack.py" pack "$IMAGES/$NAND_NAME" "$C/Resources/device/nand.itnand"
echo "$NAND_NAME" >"$C/Resources/device/nand.name"
cp "$HERE/nandpack.py" "$C/Resources/tools/"

# Stamped so an updated app re-expands instead of running on the old images,
# which is a failure nobody would think to look for.
shasum -a 256 "$C/Resources/device/nand.itnand" | cut -c1-16 >"$C/Resources/device.version"

cp "$TREE/contrib/run-ipod-touch.sh"  "$C/Resources/tools/"
cp "$TREE/contrib/it-ssh-terminal.sh" "$C/Resources/tools/"
cp "$TREE/imgtools/install-ipa.sh"    "$C/Resources/tools/"
chmod 755 "$C/Resources/tools/"*.sh

# ---------------------------------------------------------------- the launcher
#
# A compiled binary, not a script: the hardened runtime is a property of a
# Mach-O's code signature, so a bundle whose CFBundleExecutable is a script
# cannot carry it and the notary service rejects the app. The launcher only sets
# paths and execs stage-and-run.sh, which execs the runner -- so the app and the
# command line still boot the device through exactly one set of settings.
echo "==> compiling launcher"
cc -O2 -Wall -o "$C/MacOS/iPod touch" "$HERE/launcher.c"
cp "$HERE/stage-and-run.sh" "$C/Resources/tools/"
chmod 755 "$C/Resources/tools/stage-and-run.sh"

# ---------------------------------------------------------------- the metadata
cat >"$C/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>CFBundleName</key>              <string>iPod touch</string>
    <key>CFBundleDisplayName</key>       <string>iPod touch</string>
    <key>CFBundleIdentifier</key>        <string>org.qemu.ipod-touch-2g</string>
    <key>CFBundleExecutable</key>        <string>iPod touch</string>
    <key>CFBundleIconFile</key>          <string>AppIcon</string>
    <key>CFBundlePackageType</key>       <string>APPL</string>
    <key>CFBundleShortVersionString</key><string>1.0</string>
    <key>LSMinimumSystemVersion</key>    <string>11.0</string>
    <key>NSHighResolutionCapable</key>   <true/>
    <key>NSMicrophoneUsageDescription</key>
    <string>The emulated device's microphone.</string>
    <key>CFBundleDocumentTypes</key>
    <array>
        <dict>
            <key>CFBundleTypeName</key>      <string>iOS application</string>
            <key>CFBundleTypeExtensions</key><array><string>ipa</string></array>
            <key>CFBundleTypeRole</key>      <string>Viewer</string>
        </dict>
    </array>
</dict>
</plist>
PLIST

if [ -f "$HERE/AppIcon.icns" ]; then
    cp "$HERE/AppIcon.icns" "$C/Resources/AppIcon.icns"
fi

# ----------------------------------------------------------------- signing
#
# entitlements.plist has no XML comments in it, and must not gain any: codesign
# hands the file to AMFIUnserializeXML, which does not implement comments and
# fails with "syntax error near line N" pointing at the comment. So the reasons
# for the three entitlements are here instead.
#
#   allow-jit + allow-unsigned-executable-memory: QEMU's TCG translates guest
#   ARM into host code at runtime and runs it from memory it wrote itself, which
#   is exactly what the hardened runtime forbids by default -- and the hardened
#   runtime is what notarization requires. allow-jit alone is not enough: it
#   only covers MAP_JIT regions and TCG's buffer is ordinary RWX.
#
#   disable-library-validation: the GLES host path dlopens the system OpenGL
#   stack, and modules are loaded by path rather than as linked dependencies.
#
# Inside out, and explicitly. --deep is Apple-deprecated and gets nested code
# wrong often enough that the failure shows up only at the notary service,
# twenty minutes and a gigabyte of upload later.
#
# Without any signature at all the app would not even start: the dylib rewriting
# above invalidates each binary's own signature, and macOS kills a process whose
# code does not match what it is signed for.
echo "==> signing${SIGN_ID:+ as $SIGN_ID}"
SIGN_ARGS=(--force --timestamp --options runtime
           --entitlements "$HERE/entitlements.plist"
           --sign "${SIGN_ID:--}")
if [ -z "$SIGN_ID" ]; then
    # An ad-hoc signature cannot be timestamped and the hardened runtime would
    # only make the JIT entitlements load-bearing without a way to grant them.
    SIGN_ARGS=(--force --sign -)
fi

for f in "$C/Frameworks/"*.dylib; do
    codesign "${SIGN_ARGS[@]}" "$f" >/dev/null 2>&1 || die "could not sign $f"
done
[ -e "$C/Resources/tools/usbmuxd" ] && \
    codesign "${SIGN_ARGS[@]}" "$C/Resources/tools/usbmuxd" >/dev/null 2>&1
codesign "${SIGN_ARGS[@]}" "$C/MacOS/qemu-system-arm" >/dev/null 2>&1 || die "could not sign the emulator"
codesign "${SIGN_ARGS[@]}" "$APP" >/dev/null 2>&1 || die "could not sign the bundle"
codesign --verify --strict "$APP" || die "the signature does not verify"

# ------------------------------------------------------------- notarization
#
# Apple notarizes archives, not directories, and the ticket is stapled to the
# app afterwards -- so the zip that goes to the notary service is NOT the zip
# that ships. This builds a throwaway one, waits for the verdict, staples the
# ticket into the bundle, and leaves the shipping archive to be made after.
if [ -n "$NOTARIZE_PROFILE" ]; then
    [ -n "$SIGN_ID" ] || die "--notarize needs --sign: Apple will not notarize ad-hoc signed code"
    echo "==> notarizing (this uploads the whole bundle; expect several minutes)"
    tmpzip="$(mktemp -d)/notarize.zip"
    ditto -c -k --sequesterRsrc --keepParent "$APP" "$tmpzip"
    # notarytool exits 0 on a REJECTED submission -- exit status means "the
    # conversation with Apple worked", not "your app is notarized". Without
    # reading the verdict the first sign of trouble is stapler failing with
    # "Record not found", which reads like a network problem and is not one.
    subout="$(xcrun notarytool submit "$tmpzip" --keychain-profile "$NOTARIZE_PROFILE" --wait 2>&1)" || true
    echo "$subout" | tail -3
    subid="$(echo "$subout" | awk '/id: /{print $2; exit}')"
    if ! echo "$subout" | grep -q "status: Accepted"; then
        echo
        echo "build-app.sh: Apple REJECTED this build. What it objected to:" >&2
        xcrun notarytool log "$subid" --keychain-profile "$NOTARIZE_PROFILE" 2>/dev/null \
            | python3 -c "import json,sys;d=json.load(sys.stdin);print(' ',d.get('statusSummary'));[print('  -',i['path'],'--',i['message']) for i in (d.get('issues') or [])[:10]]" >&2 || true
        die "notarization failed (submission $subid)"
    fi
    xcrun stapler staple "$APP" || die "could not staple the ticket"
    rm -rf "$(dirname "$tmpzip")"
    echo "==> stapled"
fi

echo
echo "built: $APP"
du -sh "$APP" | awk '{print "size:  " $1}'
spctl -a -vv -t exec "$APP" 2>&1 | sed 's/^/gatekeeper: /' || true
