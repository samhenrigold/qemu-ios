#!/bin/bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
FLAVOR=full
OUT=""
while [ $# -gt 0 ]; do
    case "$1" in
        --flavor) FLAVOR="$2"; shift 2 ;;
        --out) OUT="$2"; shift 2 ;;
        *) echo "usage: build.sh [--flavor full|ios2] [--out DIR]" >&2; exit 1 ;;
    esac
done
case "$FLAVOR" in full|ios2) ;; *) echo "unsupported fixture flavor: $FLAVOR" >&2; exit 1 ;; esac
if [ "$FLAVOR" = ios2 ]; then
    : "${ARMV6_SDK:?ios2 requires an explicitly supplied SDK 2.0 path}"
    GUEST_API_VERSION=2.0; LEGACY_LINK=1; GUEST_ARCH=armv6
fi
. "$HERE/../armv6-toolchain/armv6.sh"
command -v ldid >/dev/null || { echo 'ldid is required for an installable IPA' >&2; exit 1; }
command -v ffmpeg >/dev/null || { echo 'ffmpeg is required for the bundled fixtures' >&2; exit 1; }
if [ -z "$OUT" ]; then
    OUT="$HERE/build"
    [ "$FLAVOR" != ios2 ] || OUT="$HERE/build-ios2"
fi
flags=()
if [ "$FLAVOR" = ios2 ]; then
    flags=(-DHARNESS_IOS2_PCM -F"$ARMV6_SDK/System/Library/Frameworks")
    mkdir -p "$OUT"
    LINK_STUB_DIR="$(mktemp -d "$OUT/link-stub.XXXXXX")"
    trap 'rm -rf "$LINK_STUB_DIR"' EXIT
    python3 "$HERE/audit.py" unused --sdk "$ARMV6_SDK" --sdk-version 2.0 --copy-link-stub "$LINK_STUB_DIR/libSystem.dylib"
    LEGACY_SYSTEM_STUB="$LINK_STUB_DIR"
fi
APP="$OUT/Payload/Harness.app"
mkdir -p "$APP"
cc6 "$HERE/harness.c" "$OUT/harness.o" -idirafter "$(xcrun clang -print-resource-dir)/include" \
    ${flags[@]+"${flags[@]}"} -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-cast-function-type-mismatch
link6 -execute "$APP/Harness" "$OUT/harness.o"
chmod 755 "$APP/Harness"
ldid -S "$APP/Harness"
# Keep the smoke run's bridge in the build directory; never replace a user's
# prebuilt helper or modify the firmware base image.
python3 "$HERE/../it-gles/genstubs.py" "$OUT/gles_stubs.h"
cc6 "$HERE/../it-gles/mbxshim.c" "$OUT/mbxshim.o" -I"$OUT" -include "$OUT/gles_stubs.h"
link6 -bundle "$OUT/MBXGLEngine" "$OUT/mbxshim.o"
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
python3 "$HERE/package.py" --flavor "$FLAVOR" "$OUT"
python3 "$HERE/package.py" --check "$OUT/Harness.ipa"
python3 "$HERE/audit.py" "$APP/Harness" --sdk "$ARMV6_SDK" $([ "$FLAVOR" = ios2 ] && echo --legacy) > "$OUT/binary-audit.json"
# Copied SDK link stub is a temporary build input, never part of the IPA.
echo "Installable app: $OUT/Harness.ipa"
