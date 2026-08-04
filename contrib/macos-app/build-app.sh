#!/bin/bash
#
# Build "iPod touch.app" -- a self-contained macOS app that boots the emulated
# device with no terminal involved.
#
#     contrib/macos-app/build-app.sh [--images <dir>] [--out <dir>]
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
# NOT NOTARIZED, and it cannot be from here -- that needs an Apple Developer ID.
# The bundle is ad-hoc signed, which is enough for it to run locally but not
# enough for Gatekeeper on someone else's Mac: they get "cannot be opened
# because the developer cannot be verified" and have to right-click ▸ Open once.
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TREE="$(cd "$HERE/../.." && pwd)"
IMAGES="${IPOD_FILES:-$HOME/Developer/qemu-ios-files}"
OUT="$TREE/build"
NAND_NAME="nand-appsync3"
QEMU="$TREE/build/qemu-system-arm"

while [ $# -gt 0 ]; do
    case "$1" in
    --images) IMAGES="$2"; shift ;;
    --out)    OUT="$2"; shift ;;
    --nand)   NAND_NAME="$2"; shift ;;
    --qemu)   QEMU="$2"; shift ;;
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
mkdir -p "$C/MacOS" "$C/Frameworks" "$C/Resources/device/ios3" "$C/Resources/tools"

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
echo "==> copying images (this is the slow part)"
cp "$IMAGES/bootrom_240_4"   "$C/Resources/device/"
cp "$IMAGES/ios3/nor_7E18.bin" "$C/Resources/device/ios3/"
cp "$IMAGES/ios3/iBoot.bin"    "$C/Resources/device/ios3/"
cp -R "$IMAGES/$NAND_NAME"     "$C/Resources/device/$NAND_NAME"

cp "$TREE/contrib/run-ipod-touch.sh"  "$C/Resources/tools/"
cp "$TREE/contrib/it-ssh-terminal.sh" "$C/Resources/tools/"
cp "$TREE/imgtools/install-ipa.sh"    "$C/Resources/tools/"
chmod 755 "$C/Resources/tools/"*.sh

# ---------------------------------------------------------------- the launcher
#
# CFBundleExecutable, and it execs the runner rather than QEMU directly, so the
# app and the command line boot the device through exactly one set of settings.
# A second copy of them here is how the two quietly drift apart.
cat >"$C/MacOS/iPod touch" <<'LAUNCHER'
#!/bin/bash
set -u
RES="$(cd "$(dirname "$0")/../Resources" && pwd)"
STATE="$HOME/Library/Application Support/iPod touch"

# The bundle is read-only (and signed), so the runner's writable areas are
# staged here instead. The images themselves are symlinked, not copied: they are
# a gigabyte, and nothing ever writes to them -- guest writes land in the
# overlay the runner creates alongside.
mkdir -p "$STATE/ios3" "$STATE/apps/work"
for f in "$RES/device/"*; do
    [ -d "$f" ] && [ "$(basename "$f")" = "ios3" ] && continue
    ln -sfn "$f" "$STATE/$(basename "$f")"
done
for f in "$RES/device/ios3/"*; do
    ln -sfn "$f" "$STATE/ios3/$(basename "$f")"
done

export IPOD_FILES="$STATE"
export QEMU="$(dirname "$0")/qemu-system-arm"
export MUXD="$RES/tools/usbmuxd"
export IT_INSTALL_IPA="$RES/tools/install-ipa.sh"
export IT_SSH_TERMINAL="$RES/tools/it-ssh-terminal.sh"
export PATH="$RES/tools:/opt/homebrew/bin:/usr/local/bin:$PATH"

# --appsync brings up USB (app install, the guest shell) and --sound sends the
# speaker to the Mac's output. Both are what someone double-clicking an app
# expects to already be on.
exec "$RES/tools/run-ipod-touch.sh" --appsync --sound "$@"
LAUNCHER
chmod 755 "$C/MacOS/iPod touch"

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

# Ad-hoc signature. Not a substitute for notarization (see the header), but
# without it the dylib rewriting above invalidates the binary's own signature
# and macOS kills the process on launch.
echo "==> signing"
codesign --force --deep --sign - "$APP" 2>/dev/null || \
    echo "    codesign failed; the app may not launch" >&2

echo
echo "built: $APP"
du -sh "$APP" | awk '{print "size:  " $1}'
