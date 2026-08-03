#!/bin/bash
#
# Install a decrypted .ipa onto a running emulator over USB, with the
# pre-flight checks that decide whether it will actually launch.
#
#     imgtools/install-ipa.sh some.ipa
#     imgtools/install-ipa.sh --check some.ipa    # pre-flight only, no device
#
# Start the emulator FIRST, with --appsync, which brings up usbmuxd before QEMU
# and records the ports it chose:
#
#     ~/Developer/qemu-ios-files/ios3/run-ios3.sh --appsync
#
# This is what the Cocoa window's drag-and-drop runs when you drop an .ipa on
# it. It is a separate script on purpose: everything here is testable from a
# terminal, which a drop handler is not.
#
# WHY THE PRE-FLIGHT CHECKS
# -------------------------
# Three things make an app install perfectly and then fail, in ways that all
# look like an emulator bug:
#
#   DTSDKName   is THE filter, not MinimumOSVersion. An app built against a
#               later SDK with an early deployment target will not launch, and
#               its Info.plist will cheerfully claim MinimumOSVersion 2.0.
#   cryptid 1   still FairPlay-encrypted. Installs, then fails its code
#               directory hash at the first __TEXT page. Needs a decrypted copy.
#   OpenGLES    launches and then wedges the device -- the MBX GPU is not
#               emulated.
#
# Each of these has already cost somebody a debugging session, so they are
# reported before the install rather than discovered after it.
set -u

die() {
    echo "install-ipa.sh: $*" >&2
    exit 1
}

CHECK_ONLY=0
case "${1:---help}" in
--help | -h)
    sed -n '3,17p' "$0" | sed 's/^# \{0,1\}//'
    exit 0
    ;;
--check)
    CHECK_ONLY=1
    shift
    ;;
esac

IPA="${1:-}"
[ -n "$IPA" ] || die "no .ipa given (try --help)"
[ -f "$IPA" ] || die "no such file: $IPA"

NAME="$(basename "$IPA")"

# ---------------------------------------------------------------- pre-flight
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
WARN=0

if ! unzip -qq -o "$IPA" 'Payload/*' -d "$TMP" 2>/dev/null; then
    die "$NAME is not a readable .ipa (no Payload/)"
fi

APP="$(find "$TMP/Payload" -maxdepth 1 -name '*.app' -type d | head -1)"
[ -n "$APP" ] || die "$NAME has no Payload/*.app"
PLIST="$APP/Info.plist"
[ -f "$PLIST" ] || die "$NAME has no Info.plist"

plist_get() {
    # Info.plist is usually binary; plutil reads both.
    plutil -extract "$1" raw -o - "$PLIST" 2>/dev/null || true
}

BUNDLE_ID="$(plist_get CFBundleIdentifier)"
SDK="$(plist_get DTSDKName)"
MINOS="$(plist_get MinimumOSVersion)"
EXE="$(plist_get CFBundleExecutable)"
BIN="$APP/${EXE:-}"

echo "--- $NAME"
echo "    bundle    ${BUNDLE_ID:-?}"
echo "    built with ${SDK:-unknown SDK}   (MinimumOSVersion ${MINOS:-?})"

# The device is 3.1.3. An SDK number ABOVE that is the failure mode; equal or
# below is fine. Compare numerically rather than as strings, or iphoneos3.2
# sorts below iphoneos3.1.3 and passes.
DEVICE_OS="${DEVICE_OS:-3.1.3}"
if [ -n "$SDK" ]; then
    SDKNUM="${SDK#iphoneos}"
    SDKNUM="${SDKNUM#iphonesimulator}"
    if [ "$(printf '%s\n%s\n' "$SDKNUM" "$DEVICE_OS" | sort -t. -k1,1n -k2,2n -k3,3n | tail -1)" \
        != "$DEVICE_OS" ] && [ "$SDKNUM" != "$DEVICE_OS" ]; then
        echo "WARNING: built against SDK $SDKNUM, newer than the device's $DEVICE_OS." >&2
        echo "  It will install and then refuse to launch. DTSDKName is the filter" >&2
        echo "  that decides this, NOT MinimumOSVersion -- which says ${MINOS:-?}." >&2
        WARN=1
    fi
fi

if [ -f "$BIN" ] && command -v otool >/dev/null; then
    if otool -l "$BIN" 2>/dev/null | grep -A6 LC_ENCRYPTION_INFO | grep -q 'cryptid 1'; then
        echo "WARNING: cryptid 1 -- still FairPlay encrypted." >&2
        echo "  It will install and then fail to launch: the code directory hash" >&2
        echo "  is checked against the first __TEXT page, which is ciphertext." >&2
        echo "  You need a decrypted (Clutch-style) copy." >&2
        WARN=1
    fi
    if otool -L "$BIN" 2>/dev/null | grep -q OpenGLES; then
        echo "WARNING: links OpenGLES." >&2
        echo "  It will install and launch, then wedge the device -- the MBX GPU" >&2
        echo "  is not emulated." >&2
        WARN=1
    fi
fi

[ "$WARN" = 0 ] && echo "    pre-flight clean"
[ "$CHECK_ONLY" = 1 ] && exit 0

# ------------------------------------------------------------------- install
#
# The session file is written by run-ios3.sh --appsync and says which usbmuxd
# belongs to which emulator. There is deliberately NO default port: several
# emulators run on this machine and they all report the same UDID (it is
# derived from our shared NOR), so guessing means an install can land on
# somebody else's device with nothing to notice it by.
SESSION="${SESSION:-$HOME/Developer/qemu-ios-files/apps/work/session.env}"
if [ -z "${SOCK:-}" ] && [ -f "$SESSION" ]; then
    # shellcheck disable=SC1090
    . "$SESSION"
fi

if [ -z "${SOCK:-}" ]; then
    cat >&2 <<EOF
install-ipa.sh: no emulator session -- usbmuxd is not running for any device.

  $SESSION does not exist, so nothing has told us which
  usbmuxd belongs to which emulator.

  Start the emulator with USB:
      \$HOME/Developer/qemu-ios-files/ios3/run-ios3.sh --appsync

  usbmuxd HAS TO BE UP BEFORE QEMU. QEMU dials out to it when the guest's USB
  core comes up, and if nothing is listening it gives up for the rest of that
  boot -- starting usbmuxd afterwards can never work, however long you wait.

  If you started the emulator another way, name the socket:
      SOCK=127.0.0.1:27015 $0 $NAME
EOF
    exit 1
fi
export USBMUXD_SOCKET_ADDRESS="$SOCK"

command -v ideviceinstaller >/dev/null ||
    die "ideviceinstaller is not on PATH (libimobiledevice)"

if ! ideviceinfo -k ProductVersion >/dev/null 2>&1; then
    echo "--- waiting for the device on $SOCK (a cold boot takes ~40 s)"
    ok=0
    for _ in $(seq 120); do
        if ideviceinfo -k ProductVersion >/dev/null 2>&1; then
            ok=1
            break
        fi
        sleep 1
    done
    [ "$ok" = 1 ] || die "no device answered on $SOCK.
    usbmuxd is listening but the guest never attached. Check that the emulator
    is still running, and \$HOME/Developer/qemu-ios-files/apps/work/usbmuxd.log."
fi
echo "--- device is up: iOS $(ideviceinfo -k ProductVersion)"

echo "--- installing $NAME"
ideviceinstaller install "$IPA"
echo "--- installed"
ideviceinstaller list --user
