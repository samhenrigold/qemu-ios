#!/bin/bash
# Relink the emulator as a dylib an iOS app can dlopen, the way UTM does.
#
# QEMU 8.2 has no --enable-shared-lib (UTM added that to their fork), so this
# reuses the exact object list ninja links qemu-system-arm from, swapping
# system/main.c -- whose main() wants the main thread for CFRunLoopRun -- for
# contrib/ios-app/qemu-ios-entry.c, which runs everything on the calling
# thread. The app dlsyms qemu_ios_main and calls it on a background pthread.
#
#     make-dylib.sh device            # real iPhone/iPad
#     TCG=interp make-dylib.sh device # the standalone interpreter build
#     make-dylib.sh sim               # iOS Simulator
#
# The backend is baked into the dylib's name so the app can carry both and
# choose at launch: only the JIT one needs a debugger.
set -eu

PLATFORM="${1:-device}"
TCG="${TCG:-jit}"
SRC="$(cd "$(dirname "$0")/../.." && pwd)"
SYSROOTS="$HOME/Developer/qemu-ios-files/ios-sysroots"

case "$PLATFORM" in
device)
    BUILD="$SRC/build-ios"
    SYSROOT="$SYSROOTS/sysroot-iOS-arm64"
    SDK="$(xcrun --sdk iphoneos --show-sdk-path)"
    TARGET="arm64-apple-ios16.0"
    ;;
sim)
    BUILD="$SRC/build-ios-sim"
    SYSROOT="$SYSROOTS/sysroot-iOS_Simulator-TCI-arm64"
    SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"
    TARGET="arm64-apple-ios16.0-simulator"
    ;;
*)
    echo "usage: $0 [device|sim]" >&2
    exit 2
    ;;
esac

case "$TCG" in
jit)    ;;
interp) BUILD="$BUILD-interp" ;;
*)      echo "TCG must be jit or interp" >&2; exit 2 ;;
esac
LIBNAME="libqemu-arm-$TCG.dylib"
OUT="$BUILD/$LIBNAME"

cd "$BUILD"

# Compile the app-facing pieces with the same include set the emulator uses.
for f in qemu-ios-entry qemu-ios-ui; do
clang -isysroot "$SDK" -target "$TARGET" -c "$SRC/contrib/ios-app/$f.c" \
    -o "$f.o" \
    -I. -I.. -Iqapi -Itrace -Iui -I"$SRC/contrib/ios-app" \
    -I"$SYSROOT/include" -I"$SYSROOT/include/pixman-1" -I"$SYSROOT/include/glib-2.0" -I"$SYSROOT/lib/glib-2.0/include" \
    -iquote . -iquote "$SRC" -iquote "$SRC/include" \
    -iquote "$SRC/host/include/aarch64" -iquote "$SRC/host/include/generic" \
    -iquote "$SRC/tcg/tci" \
    -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE_SOURCE -std=gnu11 -O2
done

# Take ninja's own link line so this cannot drift from the real build.
ninja qemu-system-arm >/dev/null
eval "LINK=($(ninja -t commands qemu-system-arm-unsigned | tail -1))"
ARGS=()
skip=0
for a in "${LINK[@]}"; do
    if [ $skip = 1 ]; then skip=0; continue; fi
    case "$a" in
        -o) skip=1 ;;                                       # drop the executable name
        *system_main.c.o) ARGS+=(qemu-ios-entry.o qemu-ios-ui.o) ;;   # main() -> qemu_ios_main(); frames out, touches in
        @*) ;;   # @block.syms / @qemu.syms export an executable's symbols; a dylib exports our entry point
        *) ARGS+=("$a") ;;
    esac
done
ARGS+=(-dynamiclib -o "$OUT" -install_name "@rpath/$LIBNAME"
      "-Wl,-exported_symbols_list,$BUILD/ios-exports.syms" -Wl,-undefined,dynamic_lookup)
printf '%q ' "${ARGS[@]}" > ios-link-dylib.sh
echo >> ios-link-dylib.sh

# Only what the app calls. Everything else stays private, which also keeps the
# dylib from exporting symbols that collide with the host app process.
cat > ios-exports.syms <<'SYMS'
_qemu_ios_main
_qemu_ios_ui_attach
_qemu_ios_ui_frame
_qemu_ios_ui_frame_size
_qemu_ios_ui_touch
_qemu_ios_ui_button
_qemu_ios_ui_input_sequence
_qemu_ios_ui_input_sequence_status
_qemu_ios_ui_input_sequence_cancel
_qemu_ios_set_foreground
_qemu_ios_snapshot_save2
_qemu_ios_snapshot_status
_qemu_ios_snapshot_resume
SYMS

bash ios-link-dylib.sh
codesign -f -s - "$OUT"
echo "built $OUT"
nm -gU "$OUT" | head
