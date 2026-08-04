#!/bin/bash
# Build qemu-system-arm for the iOS *Simulator* (arm64), as objects suitable
# for linking into an iOS app as a dylib. Modeled on how UTM SE builds QEMU
# for iOS (see github.com/utmapp/UTM scripts/build_dependencies.sh), with two
# simplifications that only work for the simulator:
#
#   * DYLD_ROOT_PATH=<simulator RuntimeRoot> makes iphonesimulator-platform
#     test binaries runnable directly on the host, so configure/meson need no
#     exe wrapper and no cross file gymnastics.
#   * The dependency sysroot is UTM's prebuilt CI artifact
#     (Sysroot-ios_simulator-tci-arm64), not something we build.
#
# TCG backend: plain TCI (--enable-tcg-interpreter), the same no-JIT story as
# UTM SE minus their TCTI speedup. Measured ~4x slower than JIT on this
# workload; fine for a first boot.
set -eu

SRC="$(cd "$(dirname "$0")/../.." && pwd)"
BUILD="$SRC/build-ios-sim"
SYSROOT="$HOME/Developer/qemu-ios-files/ios-sysroots/sysroot-iOS_Simulator-TCI-arm64"
SDK="$(xcrun --sdk iphonesimulator --show-sdk-path)"
TARGET="arm64-apple-ios16.0-simulator"

# Any Ready iOS simulator runtime works; test binaries only need dyld_sim.
RUNTIME_ROOT="$(ls -d "/Library/Developer/CoreSimulator/Volumes/"iOS_*/Library/Developer/CoreSimulator/Profiles/Runtimes/*.simruntime 2>/dev/null | tail -1)/Contents/Resources/RuntimeRoot"
[ -d "$RUNTIME_ROOT" ] || { echo "no simulator runtime root found" >&2; exit 1; }

export DYLD_ROOT_PATH="$RUNTIME_ROOT"
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig"

mkdir -p "$BUILD"
cd "$BUILD"

if [ ! -f config-host.mak ] || [ "${RECONFIGURE:-0}" = 1 ]; then
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
    --enable-tcg-interpreter \
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
