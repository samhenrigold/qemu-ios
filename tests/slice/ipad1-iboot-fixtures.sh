#!/bin/bash
# Rebuilds tests/slice/ipad1-iboot-epoch.c's fixtures on a new Mac: each iPad 1 build's decrypted iBoot at
# ~/Developer/qemu-ios-files/ipad1/repro/cache/<ipsw sha1>/iBoot.bin. The IPSW comes from LightTouchMac's firmware
# catalog (Apple's URL, checked by SHA-1) and is decrypted with its catalog keys by FirmwareKit's firmwarekit-prepare
# (LightTouchMac's FirmwareKitPrepare, from the iPhone 5 change on).
# LIGHTTOUCHMAC= the checkout (default ~/Developer/LightTouchMac); IPSW_DIR= where IPSWs are kept, named <sha1>.ipsw
# (default the app's cache, so a download is shared with it). A build whose iBoot.bin is there already is left alone.
set -euo pipefail
LTM="${LIGHTTOUCHMAC:-$HOME/Developer/LightTouchMac}"
IPSWS="${IPSW_DIR:-$HOME/Library/Caches/gold.samhenri.LightTouchMac/IPSW}"
OUT="$HOME/Developer/qemu-ios-files/ipad1/repro/cache"
CATALOG="$LTM/LightTouchMac/Resources/firmware-catalog.json"
[ -f "$CATALOG" ] || { echo "no firmware catalog at $CATALOG (set LIGHTTOUCHMAC=)"; exit 1; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/ipad1-iboot.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT
swift build --package-path "$LTM/Packages/FirmwareKit" --product firmwarekit-prepare --scratch-path "$WORK/build" >/dev/null
PREPARE="$WORK/build/debug/firmwarekit-prepare"
mkdir -p "$IPSWS"
for build in 7B500 8C148 8L1 9A405 9B206; do
    read -r sha1 url <<<"$(python3 -I -c '
import json, sys
entry = next(e for e in json.load(open(sys.argv[1]))["entries"] if e["id"] == sys.argv[2])
json.dump(entry, open(sys.argv[3], "w"))
print(entry["source"]["sha1"], entry["source"]["url"])' "$CATALOG" "k48ap-$build" "$WORK/entry.json")"
    if [ -f "$OUT/$sha1/iBoot.bin" ]; then echo "$build: present"; continue; fi
    ipsw="$IPSWS/$sha1.ipsw"
    if [ ! -f "$ipsw" ] || [ "$(shasum -a 1 "$ipsw" | cut -d' ' -f1)" != "$sha1" ]; then
        echo "$build: fetching $url"
        curl -fL --retry 3 -o "$ipsw.part" "$url"
        [ "$(shasum -a 1 "$ipsw.part" | cut -d' ' -f1)" = "$sha1" ] || { echo "$build: SHA-1 mismatch"; exit 1; }
        mv "$ipsw.part" "$ipsw"
    fi
    rm -rf "$WORK/dec"
    "$PREPARE" --decrypt "$ipsw" "$WORK/entry.json" "$WORK/dec" >"$WORK/decrypt.log" 2>&1 \
        || { cat "$WORK/decrypt.log"; exit 1; }
    mkdir -p "$OUT/$sha1"
    cp "$WORK/dec/iBoot.bin" "$OUT/$sha1/iBoot.bin"
    echo "$build: $OUT/$sha1/iBoot.bin"
done
