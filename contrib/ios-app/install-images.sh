#!/bin/bash
# Copy the emulated device's images into the app's Documents on a real iPhone.
#
#     install-images.sh [device-name-or-udid]
#
# The images are NOT in the app bundle on purpose. They are ~520 MB, and
# everything in a bundle is hashed at every signing, so bundling them would tax
# every single build with half a gigabyte of hashing. They change roughly never,
# so they are pushed once and then left alone.
#
# The NAND goes over as the packed single-file image (imgtools/pack_nand.py):
# copying 128,000 tiny files to a phone is slow enough to look hung, and the
# emulator reads the packed form ~27x faster anyway.
set -eu

APP_ID="${APP_ID:-gold.samhenri.LightTouch}"
F="$HOME/Developer/qemu-ios-files"
PACKED="${PACKED:-$F/nand-grow7g.itnand}"
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT

if [ $# -ge 1 ]; then
    DEVICE="$1"
else
    DEVICE="$(xcrun devicectl list devices --quiet --json-output /dev/stdout 2>/dev/null |
        python3 -c 'import json,sys
d = json.load(sys.stdin)["result"]["devices"]
paired = [x for x in d if x.get("connectionProperties", {}).get("pairingState") == "paired"]
print(paired[0]["hardwareProperties"]["udid"] if paired else "")')"
fi
[ -n "$DEVICE" ] || { echo "no paired device found; pass one explicitly" >&2; exit 1; }

if [ ! -f "$PACKED" ]; then
    echo "no packed NAND at $PACKED" >&2
    echo "make one:  imgtools/pack_nand.py $F/nand-grow7g $PACKED" >&2
    exit 1
fi

# Staged into one directory so this is a single copy: devicectl sets up a fresh
# connection per invocation, and one per file is painfully slow.
mkdir -p "$STAGE/qemu-ios-files/ios3"
cp "$PACKED"                "$STAGE/qemu-ios-files/nand.itnand"
cp "$F/bootrom_240_4"       "$STAGE/qemu-ios-files/bootrom_240_4"
cp "$F/ios3/nor_7E18.bin"   "$STAGE/qemu-ios-files/ios3/nor_7E18.bin"
cp "$F/ios3/iBoot.bin"      "$STAGE/qemu-ios-files/ios3/iBoot.bin"

echo "copying $(du -sh "$STAGE/qemu-ios-files" | cut -f1) to $APP_ID on $DEVICE ..."
xcrun devicectl device copy to \
    --device "$DEVICE" \
    --domain-type appDataContainer \
    --domain-identifier "$APP_ID" \
    --source "$STAGE/qemu-ios-files" \
    --destination "Documents/qemu-ios-files"

echo "done. The app looks for Documents/qemu-ios-files."
