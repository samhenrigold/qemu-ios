#!/bin/bash
#
# Inside the app bundle: put the device where it can be written to, then start
# it with everything on.
#
# The launcher binary has already exported IPOD_FILES, QEMU, MUXD and the two
# helper-tool paths. What is left is the images, which ship as one archive and
# have to be expanded before anything can run -- see build-app.sh for why they
# are an archive and not files.
set -u

RES="${IT_APP_RESOURCES:?}"
STATE="${IPOD_FILES:?}"
VERSION_FILE="$STATE/.device-version"

mkdir -p "$STATE/apps/work"

NAND_NAME="$(cat "$RES/device/nand.name")"
want="$(cat "$RES/device.version" 2>/dev/null || echo unknown)"
have="$(cat "$VERSION_FILE" 2>/dev/null || echo none)"

# COPIED, not symlinked, even though together they are only 1.2 MB.
#
# A symlink into the bundle assumes the bundle stays where it is, and on the
# first launch of a downloaded app it does not: Gatekeeper runs it from a
# randomised read-only App Translocation mount that goes away afterwards, so the
# links would point at a path that no longer exists. Copies do not care where
# the app was, or whether it is later moved to Applications.
mkdir -p "$STATE/ios3"
cp -f "$RES/device/bootrom_240_4"     "$STATE/bootrom_240_4"
cp -f "$RES/device/ios3/nor_7E18.bin" "$STATE/ios3/nor_7E18.bin"
cp -f "$RES/device/ios3/iBoot.bin"    "$STATE/ios3/iBoot.bin"

if [ "$want" != "$have" ]; then
    # The only slow thing this app ever does, and it happens with no window on
    # screen yet -- so say so, or a minute of apparently nothing is the first
    # impression. Notifications need no permission and do not steal focus.
    osascript -e 'display notification "Setting up the device. This takes about a minute, and only happens once." with title "iPod touch"' >/dev/null 2>&1 &

    # Unpacked over the top, never deleting first: the NAND overlay with the
    # user's apps and settings lives in this same directory under ios3/, and an
    # app update must not take it away.
    # ipod-helper, NOT python3: a clean macOS has no python3 (see
    # ipod-helper.c), and this is the first thing the app does, so a python
    # dependency here is a launch failure rather than a missing feature.
    if ! "${IT_HELPER:-$RES/tools/ipod-helper}" nand-unpack \
            "$RES/device/nand.itnand" "$STATE/$NAND_NAME"; then
        osascript -e 'display alert "iPod touch" message "The device images could not be unpacked. There may not be enough disk space -- about 1 GB is needed." as critical' >/dev/null 2>&1
        exit 1
    fi
    echo "$want" >"$VERSION_FILE"
fi
export NAND="$STATE/$NAND_NAME"

# The guest-side binaries (MBX GL engine replacement, home-screen placeholder).
# They ship packed because they are armv6 Mach-O and the notary service rejects
# those; see build-app.sh. Unpacked every launch -- it is 300 KB and takes
# milliseconds, and that way a repaired binary in an app update always wins over
# whatever an earlier version left here.
if [ -f "$RES/device/tools.itblob" ]; then
    if "${IT_HELPER:-$RES/tools/ipod-helper}" blob-unpack \
            "$RES/device/tools.itblob" "$STATE/guest-tools"; then
        export IT_GUEST_TOOLS="$STATE/guest-tools"
    fi
fi

# Everything on. Someone who double-clicked an app expects a working iPod, not
# a set of flags to discover: --appsync is USB (Install App..., Open Terminal)
# plus the on-screen keyboard and the code-signing gate the pasteboard daemon
# needs, --net is the emulated WiFi, --sound is the speaker.
exec "$RES/tools/run-ipod-touch.sh" --appsync --net --sound "$@"
