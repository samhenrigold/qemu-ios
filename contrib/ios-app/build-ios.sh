#!/bin/bash
# Build qemu-system-arm for iOS, as a dylib an app can dlopen. Modeled on how
# UTM builds QEMU for iOS (github.com/utmapp/UTM scripts/build_dependencies.sh),
# with one simplification: the dependency sysroot is UTM's prebuilt CI artifact
# rather than something we build ourselves.
#
#     build-ios.sh device      # real iPhone/iPad  (the target that matters)
#     build-ios.sh sim         # iOS Simulator     (convenience only)
#
# TCG BACKEND. The device build defaults to the NATIVE arm64 JIT, which is
# worth roughly 4x over the interpreter on this workload -- but it only works
# where the process is allowed to map executable pages (a development-signed
# app with a debugger attached). Set TCG=interp for the interpreter instead,
# which runs anywhere.
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
    # iphonesimulator-platform binaries run directly on the host under a
    # simulator runtime root, so configure's own test programs just work and
    # meson needs no exe wrapper.
    RUNTIME_ROOT="$(ls -d "/Library/Developer/CoreSimulator/Volumes/"iOS_*/Library/Developer/CoreSimulator/Profiles/Runtimes/*.simruntime 2>/dev/null | tail -1)/Contents/Resources/RuntimeRoot"
    [ -d "$RUNTIME_ROOT" ] || { echo "no simulator runtime root found" >&2; exit 1; }
    export DYLD_ROOT_PATH="$RUNTIME_ROOT"
    ;;
*)
    echo "usage: $0 [device|sim]" >&2
    exit 2
    ;;
esac

[ -d "$SYSROOT" ] || { echo "no sysroot at $SYSROOT" >&2; exit 1; }
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig"

if [ "$TCG" = jit ]; then
    TCG_FLAGS=""
else
    TCG_FLAGS="--enable-tcg-interpreter"
fi

mkdir -p "$BUILD"
cd "$BUILD"

if [ ! -f config-host.mak ] || [ "${RECONFIGURE:-0}" = 1 ]; then
# --cross-prefix= (empty) is what makes configure treat this as a cross build
# without prefixing every tool name; the target triple rides in --cc instead.
#
# For the DEVICE build configure cannot run its test programs -- they are
# iPhone binaries -- so anything it would probe by running has to be asserted
# here. That is why the cross build is told the CPU explicitly.
"$SRC/configure" \
    --cross-prefix= \
    --cpu=aarch64 \
    --cc="clang -isysroot $SDK -target $TARGET" \
    --cxx="clang++ -isysroot $SDK -target $TARGET" \
    --objcc="clang -isysroot $SDK -target $TARGET" \
    --host-cc=clang \
    --extra-cflags="-I$SYSROOT/include -F$SYSROOT/Frameworks" \
    --extra-ldflags="-L$SYSROOT/lib -F$SYSROOT/Frameworks $SYSROOT/lib/libcrypto.a" \
    --python=/opt/homebrew/opt/python@3.12/libexec/bin/python3 \
    --target-list=arm-softmmu \
    $TCG_FLAGS \
    --with-coroutine=sigaltstack \
    --enable-slirp \
    --audio-drv-list= --disable-coreaudio \
    --disable-hvf --disable-kvm \
    --disable-cocoa --disable-sdl --disable-sdl-image --disable-gtk \
    --disable-vnc --disable-curses --disable-brlapi \
    --disable-curl --disable-gnutls --disable-nettle --disable-gcrypt \
    --disable-auth-pam \
    --disable-libusb --disable-usb-redir \
    --disable-zstd --disable-bzip2 --disable-lzo --disable-snappy \
    --disable-png --disable-vde --disable-spice --disable-spice-protocol \
    --disable-tools --disable-guest-agent --disable-docs \
    --disable-fuse --disable-plugins --disable-xkbcommon \
    --disable-vmnet --disable-werror
fi

ninja -C "$BUILD" qemu-system-arm
