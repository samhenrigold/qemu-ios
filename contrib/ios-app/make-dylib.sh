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
#     TCG=tcti make-dylib.sh device   # the threaded interpreter build
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
tcti)   BUILD="$BUILD-tcti" ;;
*)      echo "TCG must be jit, interp or tcti" >&2; exit 2 ;;
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
ninja -t commands qemu-system-arm-unsigned | tail -1 | python3 -c '
import shlex, sys
argv = shlex.split(sys.stdin.read())
out = []
skip = False
for i, a in enumerate(argv):
    if skip:
        skip = False
        continue
    if a == "-o":
        skip = True                       # drop the executable name
        continue
    if a.endswith("system_main.c.o"):
        out.append("qemu-ios-entry.o")     # main() -> qemu_ios_main()
        out.append("qemu-ios-ui.o")        # frames out, touches in
        continue
    out.append(a)
# @block.syms / @qemu.syms are -exported_symbols_list style response files for
# an executable; a dylib must export our entry point too, so replace them.
out = [a for a in out if not a.startswith("@")]
out += ["-dynamiclib", "-o", "'"$OUT"'",
        "-install_name", "@rpath/'"$LIBNAME"'",
        "-Wl,-exported_symbols_list,'"$BUILD"'/ios-exports.syms",
        "-Wl,-undefined,dynamic_lookup"]
print(shlex.join(out))
' > ios-link-dylib.sh

# Only what the app calls. Everything else stays private, which also keeps the
# dylib from exporting symbols that collide with the host app process.
cat > ios-exports.syms <<'SYMS'
_qemu_ios_main
_qemu_ios_ui_attach
_qemu_ios_ui_frame
_qemu_ios_ui_frame_size
_qemu_ios_ui_touch
_qemu_ios_ui_button
_qemu_ios_set_foreground
_qemu_ios_snapshot_save
_qemu_ios_snapshot_done
SYMS

sh ios-link-dylib.sh
codesign -f -s - "$OUT"
echo "built $OUT"
nm -gU "$OUT" | head
