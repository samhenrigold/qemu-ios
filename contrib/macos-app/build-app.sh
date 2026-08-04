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
# The deployment target for the code built HERE -- the launcher, and what the
# emulator and usbmuxd should have been built with. 12.0 rather than something
# older because QEMU calls IOMainPort, which does not exist before macOS 12.
# What actually ships is measured from the binaries further down, not asserted.
MACOS_MIN="${MACOS_MIN:-12.0}"
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
#
# Not every dependency is recorded as an absolute path, though. otool prints
# @rpath/@loader_path entries verbatim, and a bundler that skips everything
# starting with @ drops them WITHOUT DROPPING THE REFERENCE -- the binary still
# demands the library, it just never gets copied. Six real dependencies went
# missing that way, libjxl_cms among them, and the bundle still ran here because
# the copies inherited Homebrew's own LC_RPATH and quietly reached back out to
# /opt/homebrew. On a Mac without that formula installed it is a dyld abort.
#
# So @-relative deps are resolved the way dyld would: against the LC_RPATH list
# of the binary that records them, expanded relative to where that binary
# ORIGINALLY lived -- which is why the walk carries origin paths alongside the
# copies rather than just re-reading the copy.
rpaths_of() {
    otool -l "$1" | awk '/LC_RPATH/{f=1} f&&/ path /{print $2; f=0}'
}

# The emulator links with eleven Homebrew LC_RPATHs baked in by the link line,
# and they sort ahead of the bundle's own. That is the whole reason a library
# could go missing from Frameworks and nobody notice: dyld walked past the gap
# and took the host's copy. Strip every search path that leaves the bundle, so
# what the app resolves here is exactly what it resolves on a stranger's Mac.
strip_host_rpaths() {
    local f="$1" rp
    while read -r rp; do
        case "$rp" in @loader_path*|@executable_path*) continue ;; esac
        install_name_tool -delete_rpath "$rp" "$f" 2>/dev/null || true
    done < <(rpaths_of "$f")
}

resolve_dep() {
    local dep="$1" origin="$2" odir rp cand
    odir="$(dirname "$origin")"
    case "$dep" in
    @loader_path/*|@executable_path/*)
        cand="$odir/${dep#*/}"
        [ -e "$cand" ] && { echo "$cand"; return 0; }
        return 1 ;;
    @rpath/*)
        while read -r rp; do
            case "$rp" in
                @loader_path*|@executable_path*) rp="$odir${rp#@*path}" ;;
            esac
            cand="$rp/${dep#@rpath/}"
            [ -e "$cand" ] && { echo "$cand"; return 0; }
        done < <(rpaths_of "$origin")
        return 1 ;;
    esac
    return 1
}

bundle_libs() {
    local target="$1" lib
    local -a queue=("$target|$target")

    while [ ${#queue[@]} -gt 0 ]; do
        local entry="${queue[0]}"
        queue=("${queue[@]:1}")
        local cur="${entry%%|*}" origin="${entry#*|}"

        while read -r lib; do
            case "$lib" in
                /usr/lib/*|/System/*|"") continue ;;
            esac
            # A dylib's own install id is the first line otool prints after the
            # header. It is not a dependency, it is frequently @rpath-relative
            # with no LC_RPATH that resolves it (openssl's libcrypto is), and
            # rewriting it here would undo the -id below. Skip it before trying
            # to resolve anything, or the id itself looks like a broken link.
            [ "$(basename "$lib")" = "$(basename "$cur")" ] && continue
            local src="$lib"
            case "$lib" in
                @*) src="$(resolve_dep "$lib" "$origin")" \
                        || die "cannot resolve $lib recorded in $origin" ;;
            esac
            local base; base="$(basename "$src")"
            if [ ! -e "$C/Frameworks/$base" ]; then
                cp "$src" "$C/Frameworks/$base"
                chmod u+w "$C/Frameworks/$base"
                install_name_tool -id "@rpath/$base" "$C/Frameworks/$base" 2>/dev/null || true
                queue+=("$C/Frameworks/$base|$src")
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

# ------------------------------------------------- libraries nothing links to
#
# otool -L lists what a binary LINKS. It cannot list what a binary dlopens, and
# one of these is load-bearing: Homebrew's libSDL2 is sdl2-compat, a shim whose
# entire job is to dlopen SDL3 at runtime. Nothing in the link graph mentions
# SDL3, so it was never bundled, and the app died on launch with a "Failed
# loading SDL3 library" dialog the moment it could no longer reach Homebrew.
#
# sdl2-compat looks for @loader_path/libSDL3.dylib -- next to itself -- so the
# copy has to carry that exact name, whatever the real file is called.
bundle_extra() {
    local src="$1" as="$2"
    [ -e "$src" ] || die "missing $src, which is dlopened at runtime and cannot be found by otool"
    cp "$src" "$C/Frameworks/$as"
    chmod u+w "$C/Frameworks/$as"
    install_name_tool -id "@rpath/$as" "$C/Frameworks/$as" 2>/dev/null || true
    bundle_libs "$C/Frameworks/$as"
    install_name_tool -add_rpath "@loader_path" "$C/Frameworks/$as" 2>/dev/null || true
}

if [ -e "$C/Frameworks/libSDL2-2.0.0.dylib" ] && \
   /usr/bin/strings -a "$C/Frameworks/libSDL2-2.0.0.dylib" | /usr/bin/grep -q "libSDL3.dylib"; then
    SDL3="$(brew --prefix sdl3 2>/dev/null)/lib/libSDL3.0.dylib"
    [ -e "$SDL3" ] || SDL3="/opt/homebrew/opt/sdl3/lib/libSDL3.0.dylib"
    bundle_extra "$SDL3" "libSDL3.dylib"
    echo "==> bundled SDL3 (dlopened by sdl2-compat, invisible to otool)"
fi

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
cc -O2 -Wall -mmacosx-version-min="$MACOS_MIN" -o "$C/MacOS/iPod touch" "$HERE/launcher.c"
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

# ------------------------------------------------------ the real system floor
#
# LSMinimumSystemVersion was hardcoded to 11.0 and was simply not true. Every
# Mach-O carries its own minimum in LC_BUILD_VERSION, dyld enforces THAT, and
# the app can only go back as far as the newest of them. Built on a macOS 27
# beta, the emulator came out stamped 27.0, so on any older Mac LaunchServices
# refused it with "you can't use this version of the application with this
# version of macOS" -- an error about a number nothing in the app admitted to.
#
# So measure it instead of asserting it -- but measure the right thing. dyld
# enforces minos on EXECUTABLES, not on the libraries they load: a dylib stamped
# for a macOS newer than the running one loads without complaint (verified by
# building one stamped 31.0 and loading it on 27). That matters here because the
# Homebrew dylibs carry whatever target Homebrew's build host had -- 26.0 today
# -- and if those counted, the floor could never go below the newest OS Homebrew
# has bottled for, for no reason dyld actually cares about.
#
# What gets exec'd is the launcher, the emulator and usbmuxd. Those three set the
# floor; they are ours, and they are built to MACOS_MIN.
echo "==> measuring the system floor"
minos_of() {
    otool -l "$1" 2>/dev/null | awk '
        /LC_BUILD_VERSION/    {v=1} v&&/ minos /   {print $2; exit}
        /LC_VERSION_MIN_MACOSX/{m=1} m&&/ version /{print $2; exit}'
}

FLOOR="0"
FLOOR_SETTERS=""
for bin in "$C/MacOS/iPod touch" "$C/MacOS/qemu-system-arm" "$C/Resources/tools/usbmuxd"; do
    [ -e "$bin" ] || continue
    v="$(minos_of "$bin")"
    [ -n "$v" ] || continue
    if [ "$v" = "$FLOOR" ]; then
        FLOOR_SETTERS="$FLOOR_SETTERS $(basename "$bin")"
    elif [ "$(printf '%s\n%s\n' "$FLOOR" "$v" | sort -V | tail -1)" = "$v" ]; then
        FLOOR="$v"; FLOOR_SETTERS=" $(basename "$bin")"
    fi
done

echo "    floor is macOS $FLOOR, set by:$FLOOR_SETTERS"
plutil -replace LSMinimumSystemVersion -string "$FLOOR" "$C/Info.plist"

# Every one of those three is ours and is meant to be built to MACOS_MIN, so a
# higher floor is a misconfigured build rather than a real constraint: it means
# something got compiled against the host's default target and will refuse to
# launch on any Mac older than whatever this one happens to be running.
if [ "$FLOOR" != "$MACOS_MIN" ]; then
    echo "    NOTE:$FLOOR_SETTERS holds the floor at $FLOOR, not $MACOS_MIN." >&2
    echo "          Rebuild with MACOSX_DEPLOYMENT_TARGET=$MACOS_MIN to lower it." >&2
fi

# ------------------------------------------------------- sealing the bundle
#
# Everything is staged now, so close the bundle off from the host and then prove
# it: every Mach-O in here must resolve every one of its libraries inside the
# bundle or in /usr/lib. Checked rather than assumed, because the failure this
# guards against is invisible on the machine that builds it -- the app that
# shipped needed `brew install jpeg-xl` to start, and nothing here said so.
echo "==> sealing"
while read -r bin; do
    file "$bin" | /usr/bin/grep -q "Mach-O" || continue
    strip_host_rpaths "$bin"
done < <(find "$C" -type f -perm -u+x)

missing=""
while read -r bin; do
    file "$bin" | /usr/bin/grep -q "Mach-O" || continue
    while read -r dep; do
        case "$dep" in
            /usr/lib/*|/System/*|"") continue ;;
            @rpath/*)
                [ -e "$C/Frameworks/${dep#@rpath/}" ] || missing="$missing
    $(basename "$bin") wants ${dep#@rpath/}" ;;
            @*) ;;  # @loader_path/@executable_path are self-relative, already correct
            *) missing="$missing
    $(basename "$bin") still points at $dep" ;;
        esac
    done < <(otool -L "$bin" | tail -n +2 | awk '{print $1}')
done < <(find "$C" -type f -perm -u+x)

if [ -n "$missing" ]; then
    echo "build-app.sh: the bundle is not self-contained:$missing" >&2
    die "refusing to ship a bundle that needs Homebrew"
fi
echo "    every library resolves inside the bundle"

# The link graph is now closed, but a dlopen is not in the link graph -- that is
# how SDL3 went missing. So look for the other half: any library that calls
# dlopen, and any library-shaped string inside it that names something not in
# here. It is a heuristic on strings, so it warns rather than fails; a real gap
# in this list is a launch-time crash on someone else's Mac. SDL3's optional
# GL and Vulkan backends show up here and are genuinely absent by design --
# the app displays through Cocoa and never initialises SDL video.
gaps=""
for f in "$C/Frameworks/"*.dylib; do
    nm -u "$f" 2>/dev/null | /usr/bin/grep -q "_dlopen" || continue
    while read -r n; do
        [ -e "$C/Frameworks/$n" ] || gaps="$gaps
    $(basename "$f") may dlopen $n, which is not bundled"
    done < <(/usr/bin/strings -a "$f" | /usr/bin/grep -oE "lib[A-Za-z0-9_.+-]*\.dylib" | sort -u)
done
if [ -n "$gaps" ]; then
    echo "build-app.sh: possible dlopen gaps (check before shipping):$gaps" >&2
else
    echo "    no dlopen gaps"
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
