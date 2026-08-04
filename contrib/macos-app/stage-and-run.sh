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

# Small and read-only: symlinked out of the bundle rather than copied.
mkdir -p "$STATE/ios3"
ln -sfn "$RES/device/bootrom_240_4"     "$STATE/bootrom_240_4"
ln -sfn "$RES/device/ios3/nor_7E18.bin" "$STATE/ios3/nor_7E18.bin"
ln -sfn "$RES/device/ios3/iBoot.bin"    "$STATE/ios3/iBoot.bin"

if [ "$want" != "$have" ]; then
    # The only slow thing this app ever does, and it happens with no window on
    # screen yet -- so say so, or a minute of apparently nothing is the first
    # impression. Notifications need no permission and do not steal focus.
    osascript -e 'display notification "Setting up the device. This takes about a minute, and only happens once." with title "iPod touch"' >/dev/null 2>&1 &

    # Unpacked over the top, never deleting first: the NAND overlay with the
    # user's apps and settings lives in this same directory under ios3/, and an
    # app update must not take it away.
    if ! python3 "$RES/tools/nandpack.py" unpack \
            "$RES/device/nand.itnand" "$STATE/$NAND_NAME"; then
        osascript -e 'display alert "iPod touch" message "The device images could not be unpacked. There may not be enough disk space -- about 1 GB is needed." as critical' >/dev/null 2>&1
        exit 1
    fi
    echo "$want" >"$VERSION_FILE"
fi
export NAND="$STATE/$NAND_NAME"

# Everything on. Someone who double-clicked an app expects a working iPod, not
# a set of flags to discover: --appsync is USB (Install App..., Open Terminal)
# plus the on-screen keyboard and the code-signing gate the pasteboard daemon
# needs, --net is the emulated WiFi, --sound is the speaker.
exec "$RES/tools/run-ipod-touch.sh" --appsync --net --sound "$@"
