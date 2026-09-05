#!/bin/bash
set -euo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
. "$HERE/../armv6-toolchain/armv6.sh"
command -v ldid >/dev/null || { echo 'ldid is required for an installable IPA' >&2; exit 1; }
command -v ffmpeg >/dev/null || { echo 'ffmpeg is required for the bundled fixtures' >&2; exit 1; }
OUT="$HERE/build"
APP="$OUT/Payload/Harness.app"
mkdir -p "$APP"
cc6 "$HERE/harness.c" "$OUT/harness.o" -idirafter "$(xcrun clang -print-resource-dir)/include" \
    -Wall -Wextra -Wno-unused-function -Wno-unused-parameter -Wno-cast-function-type-mismatch
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
python3 "$HERE/package.py" "$OUT"
python3 "$HERE/package.py" --check "$OUT/Harness.ipa"
echo "Installable app: $OUT/Harness.ipa"
