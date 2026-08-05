#!/bin/bash
# Put everything the emulator needs inside the app bundle: the emulator itself,
# the libraries it links, and the emulated device's images. Run from an Xcode
# "Run Script" build phase, which is why it reads PLATFORM_NAME /
# BUILT_PRODUCTS_DIR / FRAMEWORKS_FOLDER_PATH from the environment rather than
# taking arguments.
#
# iOS will not load code from outside the app bundle, so the dylibs cannot just
# be referenced where they were built (which is what the Simulator build was
# getting away with). Bundling the images too means a fresh device needs
# nothing but an install -- no side-channel copy step.
set -eu

QEMU_SRC="${QEMU_SRC:-$HOME/Developer/qemu-ios}"
SYSROOTS="$HOME/Developer/qemu-ios-files/ios-sysroots"

case "${PLATFORM_NAME:-iphoneos}" in
iphonesimulator)
    BUILD="$QEMU_SRC/build-ios-sim"
    SYSROOT="$SYSROOTS/sysroot-iOS_Simulator-TCI-arm64"
    ;;
*)
    BUILD="$QEMU_SRC/build-ios"
    SYSROOT="$SYSROOTS/sysroot-iOS-arm64"
    ;;
esac

DEST="$BUILT_PRODUCTS_DIR/$FRAMEWORKS_FOLDER_PATH"
mkdir -p "$DEST"

# Every TCG backend built ships, and the app picks at launch: the JIT one is
# far faster but iOS only allows executable memory while a debugger is
# attached, so a launch from the home screen has to fall back to an
# interpreter. JIT and TCTI are optional; plain interp is not, because it is
# the one that runs standalone on any host.
JIT_LIB="$BUILD/libqemu-arm-jit.dylib"
INTERP_LIB="$BUILD-interp/libqemu-arm-interp.dylib"
TCTI_LIB="$BUILD-tcti/libqemu-arm-tcti.dylib"

if [ ! -f "$INTERP_LIB" ] && [ ! -f "$JIT_LIB" ] && [ ! -f "$TCTI_LIB" ]; then
    echo "error: no emulator built for $PLATFORM_NAME" >&2
    echo "note: $QEMU_SRC/contrib/ios-app/build-ios.sh, then make-dylib.sh" >&2
    exit 1
fi

# The emulator, plus the transitive closure of what it links. Resolved from the
# binary rather than listed by hand, so a new dependency cannot be silently
# left out of the bundle and fail at dlopen time on the device.
#
# The recursion guard is a list of names already VISITED, not "is it already in
# the bundle": guarding on the destination meant a rebuilt emulator was never
# re-copied, so the phone quietly went on running the previous build and every
# fix looked like it had not worked.
SEEN="$(mktemp)"
trap 'rm -f "$SEEN"' EXIT

copy_with_deps() {
    lib="$1"
    name="$(basename "$lib")"

    grep -qx "$name" "$SEEN" && return 0
    echo "$name" >> "$SEEN"

    cp -f "$lib" "$DEST/$name"
    chmod +w "$DEST/$name"

    otool -L "$DEST/$name" | tail -n +2 | awk '{print $1}' |
        grep '^@rpath/' | sed 's|@rpath/||' | while read -r dep; do
            [ -f "$SYSROOT/lib/$dep" ] && copy_with_deps "$SYSROOT/lib/$dep"
        done
    return 0
}

# TCTI is built but NOT shipped by default: measured on device it was within
# 1% of the plain interpreter on this workload (104.0 vs 104.9 CPU-seconds for
# identical guest work) while costing 322 MB of gadget tables. Set
# EMBED_TCTI=1 to include it for a comparison run.
[ "${EMBED_TCTI:-0}" = 1 ] || TCTI_LIB=/nonexistent

for lib in "$JIT_LIB" "$INTERP_LIB" "$TCTI_LIB"; do
    [ -f "$lib" ] && copy_with_deps "$lib"
done

# Every Mach-O in the bundle has to carry the app's own signature, or the app
# is rejected at launch.
for f in "$DEST"/*.dylib; do
    [ -f "$f" ] || continue
    codesign --force --sign "${EXPANDED_CODE_SIGN_IDENTITY:--}" \
        ${OTHER_CODE_SIGN_FLAGS:-} --timestamp=none "$f" >/dev/null
done

echo "embedded $(ls "$DEST"/*.dylib | wc -l | tr -d ' ') dylibs from $BUILD"

# --- the emulated device's images -----------------------------------------
#
# The base NAND goes in as the packed single-file image: a directory of 128,000
# page files would be both slower to read and painfully slow to install, and
# the packed form is read ~27x faster (see imgtools/pack_nand.py).
#
# These are ~520 MB and change roughly never, so each is copied only when the
# source is newer -- otherwise every build would spend a minute re-copying half
# a gigabyte. Signing still hashes them, which is the price of bundling.
IMAGES="$BUILT_PRODUCTS_DIR/$UNLOCALIZED_RESOURCES_FOLDER_PATH/qemu-ios-files"
F="$HOME/Developer/qemu-ios-files"
PACKED="${PACKED:-$F/nand-grow7g.itnand}"

if [ ! -f "$PACKED" ]; then
    echo "warning: no packed NAND at $PACKED; the app will have no device to boot" >&2
    echo "note: make one with $QEMU_SRC/imgtools/pack_nand.py $F/nand-grow7g $PACKED" >&2
else
    mkdir -p "$IMAGES/ios3"
    install_if_newer() {
        [ -f "$1" ] || { echo "warning: missing $1" >&2; return 0; }
        if [ ! -f "$2" ] || [ "$1" -nt "$2" ]; then
            cp -f "$1" "$2"
        fi
    }
    install_if_newer "$PACKED"              "$IMAGES/nand.itnand"
    install_if_newer "$F/bootrom_240_4"     "$IMAGES/bootrom_240_4"
    install_if_newer "$F/ios3/nor_7E18.bin" "$IMAGES/ios3/nor_7E18.bin"
    install_if_newer "$F/ios3/iBoot.bin"    "$IMAGES/ios3/iBoot.bin"
    echo "bundled images: $(du -sh "$IMAGES" | cut -f1)"
fi
