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
#   mode 0644   the app's binary as stored in the .ipa. installd extracts it
#               faithfully, posix_spawn then fails with EACCES, and the icon
#               bounces once with NO crash report written -- so it reads as a
#               crash that left no evidence. Repacked 0755 here.
#   OpenGLES    the stock MBXGLEngine drives the MBX GPU, which is not
#               emulated, and the app wedges the whole device. Our replacement
#               (contrib/it-gles) forwards GL to the host instead, and is
#               installed automatically when the app links OpenGLES.
#
# Each of these has already cost somebody a debugging session, so they are
# handled or reported before the install rather than discovered after it.
set -u

die() {
    echo "install-ipa.sh: $*" >&2
    exit 1
}

# Copy an .ipa, setting one member's archived unix mode to 0755. Only the mode
# word changes; every byte of every member is carried across unaltered, which
# matters because the code directory hashes the file contents.
repack_mode() {
    python3 - "$1" "$2" "$3" <<'PY'
import shutil, sys, zipfile

src, dst, want = sys.argv[1:4]
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as zout:
    for info in zin.infolist():
        data = zin.read(info)
        if info.filename == want:
            # external_attr's high 16 bits are st_mode. Keep the file type
            # bits (0o100000) and give it 0755.
            info.external_attr = (0o100755 << 16) | (info.external_attr & 0xFFFF)
        zout.writestr(info, data)
PY
}

# --------------------------------------------------------------- guest shell
#
# Two things here need a shell on the device rather than AFC: the GL engine
# replacement lives in /System, which AFC cannot reach, and the home-screen
# placeholder is a program that has to run on the guest. Both go through one
# ssh tunnel, opened on demand and closed by the EXIT trap -- opening a second
# iproxy on the same port silently fails and then every command "just" times
# out, which is a bad way to spend an afternoon.
GUEST_PORT=""
GUEST_IPROXY=""
GUEST_SSH_OPTS=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null
    -o LogLevel=ERROR -o PreferredAuthentications=password
    -o ConnectTimeout=10)

guest_open() {
    [ -n "$GUEST_IPROXY" ] && return 0
    command -v iproxy >/dev/null || {
        echo "install-ipa.sh: iproxy is not on PATH (libimobiledevice)" >&2
        return 1
    }
    GUEST_PORT="${SSH_PORT:-2239}"
    iproxy "$GUEST_PORT" 22 >/dev/null 2>&1 &
    GUEST_IPROXY=$!
    sleep 2
    if ! kill -0 "$GUEST_IPROXY" 2>/dev/null; then
        GUEST_IPROXY=""
        echo "install-ipa.sh: iproxy would not start on port $GUEST_PORT" >&2
        return 1
    fi

    # The device's root password. `ssh -o PreferredAuthentications=password`
    # will not read a password from a pipe, so it goes through SSH_ASKPASS.
    local ask="$TMP/askpass"
    printf '#!/bin/sh\necho %s\n' "${DEVICE_PASSWORD:-alpine}" >"$ask"
    chmod 755 "$ask"
    export SSH_ASKPASS="$ask" SSH_ASKPASS_REQUIRE=force DISPLAY="${DISPLAY:-:0}"
    return 0
}

guest_close() {
    [ -n "$GUEST_IPROXY" ] || return 0
    # Job control announces the kill on the terminal ("Terminated: 15") unless
    # the job is disowned first, which reads as an error in the middle of a
    # successful install.
    disown "$GUEST_IPROXY" 2>/dev/null || true
    kill "$GUEST_IPROXY" 2>/dev/null
    GUEST_IPROXY=""
}

guest_sh() {
    ssh "${GUEST_SSH_OPTS[@]}" -p "$GUEST_PORT" root@127.0.0.1 "$@" >/dev/null 2>&1
}

guest_put() {
    scp -O "${GUEST_SSH_OPTS[@]}" -P "$GUEST_PORT" "$1" "root@127.0.0.1:$2" >/dev/null 2>&1
}

# Put our MBXGLEngine.bundle replacement on the device, keeping the stock one
# next to it as MBXGLEngine.stock. Over ssh, because AFC is confined to the
# media partition and this file lives in /System.
install_shim() {
    guest_open || return 1
    local B=/System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle

    guest_put "$SHIM" /tmp/MBXGLEngine.new &&
        guest_sh "cp -n $B/MBXGLEngine $B/MBXGLEngine.stock; cp /tmp/MBXGLEngine.new $B/MBXGLEngine && chmod 755 $B/MBXGLEngine"
}

# ------------------------------------------------------- home-screen feedback
#
# A real App Store install puts a placeholder icon on the home screen the
# moment the download starts and swaps in the real icon when it finishes.
# SpringBoard will do that for us from an ordinary guest process, with no
# injection and no entitlement: contrib/it-instprogress/sbdlicon calls
# SBAddDownloadingIconForDisplayIdentifier on SpringBoard's own server port.
# The README there has the reverse engineering, including why the bundle id it
# passes is the placeholder's own display identifier rather than the app's.
#
# This is deliberately best-effort. Nothing about the install depends on it, so
# every failure here is a warning and the install carries on -- but a
# placeholder that was placed is ALWAYS taken down again, success or failure,
# by the EXIT trap. A placeholder left sitting on the home screen for an app
# that never arrived is worse than no placeholder at all.
#
# The progress bar stays empty. Filling it needs an ISDownload object that only
# itunesstored can publish; see the README. The label is SpringBoard's own
# "Waiting…", which is what iOS 3 really shows, so nothing here is faked.
PLACEHOLDER=""

placeholder_add() {
    local tool="$REPO/contrib/it-instprogress/sbdlicon" id="$1"

    [ -f "$tool" ] || {
        echo "install-ipa.sh: no $tool, so no home-screen placeholder" >&2
        echo "  build it with contrib/it-instprogress/build.sh" >&2
        return 1
    }
    guest_open || return 1
    guest_put "$tool" /tmp/sbdlicon || return 1
    guest_sh "chmod 755 /tmp/sbdlicon && /tmp/sbdlicon add '$id'" || return 1
    PLACEHOLDER="$id"
}

placeholder_remove() {
    [ -n "$PLACEHOLDER" ] || return 0
    local id="$PLACEHOLDER"
    # Cleared FIRST, so a failure here cannot loop or double-report. If the
    # guest has become unreachable the icon stays until the next respring --
    # SpringBoard places it with saveIconState:NO, so it is never written to
    # disk and cannot outlive the running SpringBoard.
    PLACEHOLDER=""
    guest_sh "/tmp/sbdlicon cancel '$id'" ||
        echo "install-ipa.sh: could not take the placeholder icon down; it will
  go away by itself on the next respring" >&2
}

cleanup() {
    placeholder_remove
    guest_close
    rm -rf "$TMP"
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
# INT and TERM as well as EXIT: a Ctrl-C part way through an install is exactly
# when a placeholder would otherwise be orphaned on the home screen.
trap cleanup EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
WARN=0
LINKS_GLES=0
REPO="$(cd "$(dirname "$0")/.." && pwd)"
# Our MBXGLEngine.bundle replacement, built by contrib/it-gles/build.sh. It is
# not committed -- it is an armv6 Mach-O bundle produced from committed source.
SHIM="${SHIM:-$REPO/contrib/it-gles/MBXGLEngine}"

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
        LINKS_GLES=1
        if [ -f "$SHIM" ]; then
            echo "    links OpenGLES -- the GL engine replacement will be installed"
        else
            echo "WARNING: links OpenGLES, and $SHIM does not exist." >&2
            echo "  The stock MBXGLEngine drives the PowerVR MBX, which is not" >&2
            echo "  emulated: the app launches, draws its UIKit chrome once, and" >&2
            echo "  then WEDGES THE WHOLE DEVICE spinning on an MBX register that" >&2
            echo "  never acknowledges. Build the replacement first:" >&2
            echo "      contrib/it-gles/build.sh" >&2
            WARN=1
        fi
    fi
fi

# An .ipa is a zip and installd extracts it preserving the archived mode, so an
# app whose binary is stored 0644 arrives on the device not executable. That is
# not a crash and it produces no crash report: posix_spawn fails with EACCES,
# SpringBoard says only "exited abnormally with exit status 1", and the icon
# bounces once. It is a common shape in 2009-era .ipas -- Cube Runner is one --
# and it looks exactly like an emulator bug, which is why it is repaired here
# rather than left to a chmod over ssh afterwards.
NEEDS_MODE=0
if [ -f "$BIN" ] && [ ! -x "$BIN" ]; then
    echo "    NOTE: $EXE is stored 0644 in the archive; repacking it 0755"
    NEEDS_MODE=1
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
#
# IPOD_FILES is where the runner keeps its state, and the packaged app moves it
# to Application Support because an app bundle is read-only. Honouring it here
# is what makes Install App... work inside the app: without it this looks in a
# home directory the app never writes to, and reports "usbmuxd is not running"
# about a usbmuxd that is running perfectly well a few directories away.
SESSION="${SESSION:-${IPOD_FILES:-$HOME/Developer/qemu-ios-files}/apps/work/session.env}"
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
      contrib/run-ipod-touch.sh --appsync
  (or just launch the packaged app, which passes --appsync for you)

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

# The GL engine has to be in place BEFORE the app is launched, and it is worth
# doing before the install too: a wedged device cannot finish an scp, so the
# first launch is the last chance to get anything onto it.
if [ "$LINKS_GLES" = 1 ] && [ -f "$SHIM" ]; then
    echo "--- installing the GL engine replacement"
    install_shim || echo "install-ipa.sh: could not install the GL engine; the app will wedge the device" >&2
fi

# The placeholder goes up BEFORE the install, which is the whole point of it:
# an install takes tens of seconds and until now the home screen said nothing
# at all was happening. Keyed on the bundle id so dropping the same .ipa twice
# reuses the one icon instead of stacking them up.
if [ -n "$BUNDLE_ID" ]; then
    placeholder_add "qemu-install-$BUNDLE_ID" ||
        echo "install-ipa.sh: continuing without the home-screen placeholder" >&2
fi

echo "--- installing $NAME"
if [ "$NEEDS_MODE" = 1 ]; then
    FIXED="$TMP/$NAME"
    repack_mode "$IPA" "$FIXED" "Payload/$(basename "$APP")/$EXE"
    ideviceinstaller install "$FIXED"
else
    ideviceinstaller install "$IPA"
fi
RC=$?

# Down it comes either way. On success SpringBoard has already put the real
# icon on the home screen -- installd announces the install and SpringBoard
# adds it without being asked -- so the placeholder has served its purpose;
# on failure it must not be left claiming an app is on the way.
placeholder_remove

[ "$RC" = 0 ] || die "ideviceinstaller failed ($RC); nothing was installed"
echo "--- installed"
ideviceinstaller list --user
