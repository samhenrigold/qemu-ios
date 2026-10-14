#!/bin/bash
# Native macOS build with the wasm build's device set: no FFmpeg (it adds h264 vmstate fields,
# so a state saved with it won't load in the wasm build), no Cocoa/CoreAudio/SDL/slirp, no GLES bridge (its vmstate section is absent from the wasm build).
# usage: build-native.sh QEMU_SRC BUILD_DIR [extra configure args, e.g. --enable-tcg-interpreter]
set -euo pipefail
SRC=$1 B=$2; shift 2
PC=$HOME/Developer/wasm-spike-deps/native-pc   # Homebrew's .pc files minus libav*/libsw*
mkdir -p "$PC"
for f in /opt/homebrew/lib/pkgconfig/*.pc /opt/homebrew/share/pkgconfig/*.pc /opt/homebrew/opt/libffi/lib/pkgconfig/*.pc; do
  case $(basename "$f") in libav* | libsw* | libpostproc*) ;; *) ln -sf "$f" "$PC/" ;; esac
done
mkdir -p "$B"; cd "$B"
[ -f build.ninja ] || PKG_CONFIG_LIBDIR=$PC "$SRC/configure" --python=/opt/homebrew/opt/python@3.12/libexec/bin/python3 \
  --target-list=arm-softmmu --disable-cocoa --disable-coreaudio --disable-sdl --disable-slirp --disable-capstone \
  --disable-pie --disable-fuse --disable-docs --disable-opengl --extra-cflags=-I/opt/homebrew/opt/openssl@3/include \
  "--extra-ldflags=-L/opt/homebrew/opt/openssl@3/lib -lcrypto" "$@"
nice ninja -j8 qemu-system-arm
! grep -q IT_HAVE_AVCODEC build.ninja
grep -q gles-host-stub build.ninja
