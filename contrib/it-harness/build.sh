#!/bin/bash
# Build the test harness app and its installable IPA: build.sh [--out DIR] (default contrib/it-harness/build).
# LightTouchMac's `sessions single` installs build/Harness.ipa from the pinned checkout.
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
OUT="$HERE/build"
case "${1:-}" in
    --out) OUT="$2" ;;
    "") ;;
    *) echo "usage: build.sh [--out DIR]" >&2; exit 1 ;;
esac
. "$HERE/../armv6-toolchain/armv6.sh"
command -v ldid >/dev/null || { echo 'ldid is required for an installable IPA' >&2; exit 1; }
command -v ffmpeg >/dev/null || { echo 'ffmpeg is required for the bundled fixtures' >&2; exit 1; }
APP="$OUT/Payload/Harness.app"
mkdir -p "$APP"
cc6 "$HERE/harness.c" "$OUT/harness.o" -idirafter "$(xcrun clang -print-resource-dir)/include" \
    -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-cast-function-type-mismatch
link6 -execute "$APP/Harness" "$OUT/harness.o"
chmod 755 "$APP/Harness"
ldid -S "$APP/Harness"
ffmpeg -hide_banner -loglevel error -y -f lavfi \
    -i 'aevalsrc=0.2*sin(2*PI*440*t)|0.2*sin(2*PI*880*t):s=44100:d=6' \
    -c:a pcm_s16le "$APP/stereo.wav"
for spec in 'aac aac.m4a' 'libmp3lame tone.mp3' 'alac lossless.m4a'; do
    read -r codec name <<< "$spec"
    ffmpeg -hide_banner -loglevel error -y -i "$APP/stereo.wav" -c:a "$codec" "$APP/$name"
done
for codec in h264 mpeg4; do
    flags=(-c:v mpeg4 -q:v 4)
    if [ "$codec" = h264 ]; then
        flags=(-c:v libx264 -profile:v baseline -level:v 3.0 -bf 0 -g 30 -pix_fmt yuv420p)
    fi
    ffmpeg -hide_banner -loglevel error -y -f lavfi -i 'testsrc2=size=320x240:rate=30:duration=6' \
        -i "$APP/stereo.wav" "${flags[@]}" -c:a aac -b:a 96k -shortest -movflags +faststart "$APP/$codec.mp4"
done
cat > "$APP/Info.plist" <<'PLIST'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>CFBundleDisplayName</key>
	<string>Test Harness</string>
	<key>CFBundleExecutable</key>
	<string>Harness</string>
	<key>CFBundleIdentifier</key>
	<string>com.qemuios.harness</string>
	<key>CFBundleInfoDictionaryVersion</key>
	<string>6.0</string>
	<key>CFBundleName</key>
	<string>Harness</string>
	<key>CFBundlePackageType</key>
	<string>APPL</string>
	<key>CFBundleSupportedPlatforms</key>
	<array>
		<string>iPhoneOS</string>
	</array>
	<key>CFBundleVersion</key>
	<string>1.0</string>
	<key>DTPlatformName</key>
	<string>iphoneos</string>
	<key>DTSDKName</key>
	<string>iphoneos3.1.3</string>
	<key>LSRequiresIPhoneOS</key>
	<true/>
	<key>MinimumOSVersion</key>
	<string>3.1</string>
	<key>UIStatusBarHidden</key>
	<false/>
</dict>
</plist>
PLIST
# An armv6 executable with the 2010 entry point (LC_UNIXTHREAD, no LC_MAIN) and a signature.
cmds="$(xcrun otool -l "$APP/Harness" | awk '$1 == "cmd" { print $2 }')"
xcrun otool -h "$APP/Harness" | awk 'NR == 4 && !($2 == 12 && $3 == 6 && $5 == 2) { exit 1 }' ||
    { echo "Harness is not an armv6 executable" >&2; exit 1; }
for want in LC_UNIXTHREAD LC_CODE_SIGNATURE; do
    grep -qx "$want" <<<"$cmds" || { echo "Harness lacks $want" >&2; exit 1; }
done
! grep -qx LC_MAIN <<<"$cmds" || { echo "Harness has LC_MAIN (old dyld refuses it)" >&2; exit 1; }
rm -f "$OUT/Harness.ipa"
(cd "$OUT" && zip -q -X Harness.ipa Payload/Harness.app/Harness Payload/Harness.app/Info.plist \
    Payload/Harness.app/{stereo.wav,aac.m4a,tone.mp3,lossless.m4a,h264.mp4,mpeg4.mp4})
rm -f "$OUT/harness.o"
echo "Installable app: $OUT/Harness.ipa"
