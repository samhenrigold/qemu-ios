#!/bin/bash
# Build qemu-system-arm for iOS, as a dylib an app can dlopen. Modeled on how
# UTM builds QEMU for iOS (github.com/utmapp/UTM scripts/build_dependencies.sh),
# with one simplification: the dependency sysroot is UTM's prebuilt CI artifact
# rather than something we build ourselves.
#
#     build-ios.sh device      # real iPhone/iPad  (the target that matters)
#     build-ios.sh sim         # iOS Simulator     (convenience only)
#
# TCG BACKEND, selected with TCG=jit (default), TCG=interp or TCG=tcti. All
# are worth building: the app ships them and picks at launch.
#
#   jit     native arm64 codegen, ~4x faster, but iOS only permits executable
#           memory while a debugger is attached, so it is development-only.
#   interp  no executable memory at all, so it runs standalone from the home
#           screen -- which is the only way an ordinary launch can work.
#   tcti    UTM's threaded-code interpreter: also no executable memory, but it
#           dispatches through precompiled aarch64 gadgets instead of a switch,
#           so it is a lot faster than interp. aarch64 hosts only.
#
# Each backend gets its own build directory (build-ios, build-ios-interp,
# build-ios-tcti).
#
# Optimisation flags (PGO_PROFILE / PGO_GENERATE, see below) only reach the
# build through configure, so changing them needs RECONFIGURE=1.
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

case "$TCG" in
jit)    ;;
interp) BUILD="$BUILD-interp" ;;
tcti)   BUILD="$BUILD-tcti" ;;
*)      echo "TCG must be jit, interp or tcti" >&2; exit 2 ;;
esac

[ -d "$SYSROOT" ] || { echo "no sysroot at $SYSROOT" >&2; exit 1; }
export PKG_CONFIG_LIBDIR="$SYSROOT/lib/pkgconfig"

case "$TCG" in
jit)    TCG_FLAGS="" ;;
interp) TCG_FLAGS="--enable-tcg-interpreter" ;;
tcti)   TCG_FLAGS="--enable-tcg-threaded-interpreter" ;;
esac

# OPTIMISATION FLAGS.
#
# Measured on a macOS build of the same TCI interpreter (build-tci-mac and
# friends), timing a cold boot to a fixed guest milestone -- the only workload
# on hand that is actually compute-bound:
#
#   -O3                         within noise of the default -O2  (-0.6%)
#   -O3 -flto=thin / -flto       within noise                    (+0.3% / -1.3%)
#   -O3 -mcpu=<apple core>       within noise                    (-2.9%)
#   -O3 -fprofile-use            13.7% faster, reproducible
#
# So PGO is the only one of the four that pays, and it pays properly: the TCI
# dispatch loop is one enormous switch, and the profile is what tells clang
# which arms are hot and how to lay the branch tree out. Everything else is
# left off rather than carried as cargo.
#
#   PGO_GENERATE=1   build instrumented, to collect a profile
#   PGO_PROFILE=path build against an existing .profdata
#
# Collecting the profile: -fprofile-generate needs the process to EXIT CLEANLY
# to write its .profraw (a SIGKILL loses it entirely), and it needs somewhere
# writable to put it -- on device, set LLVM_PROFILE_FILE to a path inside the
# app container.
#
# CROSS-BOUNDARY REUSE -- a profile collected from the macOS TCI build is valid
# here, and this is not a guess. clang's IR instrumentation is inserted before
# target lowering and keyed on function name plus a structural hash of the CFG,
# neither of which depends on the target triple. Verified directly: compiling
# tcg/tci.c for arm64-apple-ios16.0 against the macOS-collected profile grows
# the object 93,968 -> 111,904 bytes, the same delta the profile produces on
# macOS, and clang emits no -Wprofile-instr-out-of-date or
# -Wprofile-instr-unprofiled diagnostics for tci.c, cputlb.c or cpu-exec.c.
# The two configurations differ only in features the iOS build switches OFF
# (vnc/sdl/cocoa), so nothing on the hot path is missing from the profile.
#
# Two real limits on that reuse:
#   - It is only valid for TCG=interp. The tcti backend runs different code
#     (tcg/tcti), which the macOS TCI profile never executed, so tcti needs its
#     own profile collected against a tcti build.
#   - Functions absent from the profile are treated as cold. That is harmless
#     for iOS-only glue, but it means the profile must be REGENERATED whenever
#     the hot files change shape, or clang silently starts optimising a changed
#     hot function for size.
OPT_CFLAGS="-O3"
OPT_LDFLAGS=""
if [ "${PGO_GENERATE:-0}" = 1 ]; then
    OPT_CFLAGS="$OPT_CFLAGS -fprofile-generate"
    OPT_LDFLAGS="$OPT_LDFLAGS -fprofile-generate"
elif [ -n "${PGO_PROFILE:-}" ]; then
    [ -f "$PGO_PROFILE" ] || { echo "no profile at $PGO_PROFILE" >&2; exit 1; }
    OPT_CFLAGS="$OPT_CFLAGS -fprofile-use=$PGO_PROFILE"
    # The iOS build compiles files the profile run never entered; that is
    # expected here, so do not let it drown the build log.
    OPT_CFLAGS="$OPT_CFLAGS -Wno-profile-instr-out-of-date -Wno-profile-instr-unprofiled"
fi

# An explicit build directory, so a variant (say a PGO one) can be built and
# compared without disturbing the established build-ios* trees.
BUILD="${BUILD_DIR:-$BUILD}"

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
    --extra-cflags="-I$SYSROOT/include -F$SYSROOT/Frameworks $OPT_CFLAGS" \
    --extra-ldflags="-L$SYSROOT/lib -F$SYSROOT/Frameworks $SYSROOT/lib/libcrypto.a $OPT_LDFLAGS" \
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
    --disable-vmnet --disable-werror \
    --disable-qom-cast-debug \
    --enable-trace-backends=nop \
    --disable-virglrenderer --disable-opengl
fi

ninja -C "$BUILD" qemu-system-arm
