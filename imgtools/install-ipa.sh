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
    # ipod-helper in the app bundle -- a clean macOS has no python3. It patches
    # the central directory's mode word in place rather than rebuilding the
    # zip, so the compressed bytes are carried across untouched by
    # construction, which matters because the code directory hashes them.
    if [ -x "${IT_HELPER:-}" ]; then
        "$IT_HELPER" ipa-chmod "$1" "$2" "$3"
        return
    fi
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
    # A fixed port here was a single point of failure for every install on the
    # machine: one leaked iproxy (SIGKILLed script, crashed app) held 2239 and
    # every later install silently lost its ssh tunnel -- which for a GL app
    # means the engine replacement never lands and the app wedges the device.
    # Try the preferred port first so nothing else changes, then walk.
    for GUEST_PORT in "${SSH_PORT:-2239}" 2241 2243 2245 2247; do
        iproxy "$GUEST_PORT" 22 >/dev/null 2>&1 &
        GUEST_IPROXY=$!
        sleep 0.2
        kill -0 "$GUEST_IPROXY" 2>/dev/null && break
        GUEST_IPROXY=""
    done
    [ -n "$GUEST_IPROXY" ] || {
        echo "install-ipa.sh: iproxy would not start on any port" >&2
        return 1
    }

    # Poll for the port instead of sleeping a flat 2 s. Everything the guest
    # does is behind this, including the home-screen placeholder, and a second
    # spent here is a second the home screen says nothing is happening.
    for _ in $(seq 40); do
        kill -0 "$GUEST_IPROXY" 2>/dev/null || break
        nc -z 127.0.0.1 "$GUEST_PORT" 2>/dev/null && break
        sleep 0.1
    done
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

    # Share one connection for every command that follows. An install makes at
    # least four (put sbdlicon, run it, copy MBXGLEngine, take the placeholder
    # down) and on an emulated 400 MHz ARM the SSH handshake -- not the copy --
    # is what each one costs, several seconds of RSA and key exchange apiece.
    # Multiplexed, only the first pays it. Set here rather than in the array
    # above because ControlPath needs $TMP, which does not exist that early.
    # ControlPath is a Unix socket and macOS caps those at ~104 bytes; $TMP
    # lives under /var/folders/<deep>/T/, and "$TMP/ssh-%C" (a 40-hex hash)
    # overflowed that -- at which point ssh refuses to run AT ALL, not just
    # without multiplexing. Every guest command failed silently for as long
    # as that path was in place: no placeholder, no sbdlicon, and no GL
    # engine, which is how GL apps came to be installed in a state that
    # wedged the device. Short, fixed, pid-unique: one master per run is
    # exactly right since a run only ever talks to one guest.
    GUEST_SSH_OPTS+=(-o ControlMaster=auto -o ControlPath="/tmp/it-ssh.$$"
        -o ControlPersist=120)

    # Prove the tunnel with a real round trip before reporting it open. sshd
    # is one of the last things a fresh boot starts -- lockdown answers
    # queries a good minute before it -- and every guest_sh/guest_put above
    # this discards stderr, so an ssh that was never going to work used to
    # fail silently and read as "the placeholder/engine step is broken".
    # This also warms the ControlMaster, so the commands that follow are free.
    local err="" _t
    for _t in $(seq 18); do
        err=$(ssh "${GUEST_SSH_OPTS[@]}" -p "$GUEST_PORT" root@127.0.0.1 true 2>&1) && return 0
        sleep 5
    done
    echo "install-ipa.sh: no ssh answer from the guest after 90s: ${err:-no error output}" >&2
    guest_close
    return 1
}

guest_close() {
    [ -n "$GUEST_IPROXY" ] || return 0
    # Drop the multiplexed master first: it outlives this script by
    # ControlPersist, and a master still holding a tunnel whose iproxy has been
    # killed makes the NEXT install's first command hang until it times out.
    ssh "${GUEST_SSH_OPTS[@]}" -p "$GUEST_PORT" -O exit root@127.0.0.1 >/dev/null 2>&1 || true
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
    local tool="${IT_GUEST_TOOLS:-$REPO/contrib/it-instprogress}/sbdlicon" id="$1"
    [ -f "$tool" ] || tool="$REPO/contrib/it-instprogress/sbdlicon"

    [ -f "$tool" ] || {
        echo "install-ipa.sh: no $tool, so no home-screen placeholder" >&2
        echo "  build it with contrib/it-instprogress/build.sh" >&2
        return 1
    }
    guest_open || return 1
    # Already there from an earlier install this boot? /tmp does not survive a
    # reboot, so a hit means the copy would be byte-for-byte the same one.
    guest_sh "test -x /tmp/sbdlicon" ||
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

# ------------------------------------------------------------------- install
#
# "Could not start com.apple.afc: Invalid service" is lockdownd declining to
# spawn afcd, not a bad .ipa: it comes and goes on a 128 MB device, and most
# often right after something else has just used a service (our own ssh copy of
# the GL engine, or an uninstall). Retried here rather than one level up in the
# app, because the app's retry re-runs the entire pre-flight -- unpacking the
# .ipa, re-copying MBXGLEngine, taking the placeholder down and putting it back
# -- to arrive at the same one command. The waits get longer each time; lockdown
# has usually recovered by the second.
#
# Only service-startup failures are retried. A rejected .ipa fails the same way
# every time and should say so immediately.
install_retry() {
    local out rc=1 delay
    # The ladder reaches past a minute on purpose: right after a fresh boot,
    # or an uninstall, lockdown can refuse services for tens of seconds, and
    # an install that fails at 11s (the old 0/3/8 ceiling) reported a healthy
    # device as broken. Only the known-transient refusals retry at all.
    for delay in 0 3 8 15 30; do
        if [ "$delay" != 0 ]; then
            echo "--- lockdown would not start the service; retrying in ${delay}s"
            sleep "$delay"
        fi
        out="$(ideviceinstaller install "$1" 2>&1)"
        rc=$?
        printf '%s\n' "$out"
        [ "$rc" = 0 ] && return 0
        case "$out" in
        *"Invalid service"* | *"Could not start"* | *"Could not connect"*) ;;
        *) return "$rc" ;;
        esac
    done
    return "$rc"
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
# IT_GUEST_TOOLS is where the app unpacks the guest binaries it ships (see
# stage-and-run.sh). Run from a source tree there is no such directory and the
# repo copies are used instead, so both work.
SHIM="${SHIM:-${IT_GUEST_TOOLS:-$REPO/contrib/it-gles}/MBXGLEngine}"
[ -f "$SHIM" ] || SHIM="$REPO/contrib/it-gles/MBXGLEngine"

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

# NOT `command -v otool`. /usr/bin/otool is the SAME Command Line Tools shim as
# /usr/bin/python3 -- one of ~78 hard links to one binary -- so on a Mac without
# the developer tools the guard PASSES and both pipelines then return nothing.
# That silently set LINKS_GLES=0 for a GL app: the engine replacement was never
# installed, nothing was printed, and the device wedged on first launch. The
# helper reads the load commands itself and is always present in the app.
MACHO_INFO=""
if [ -f "$BIN" ]; then
    if [ -x "${IT_HELPER:-}" ]; then
        MACHO_INFO="$("$IT_HELPER" macho-info "$BIN" 2>/dev/null)"
    elif command -v otool >/dev/null && otool -h "$BIN" >/dev/null 2>&1; then
        # Source tree with real developer tools: otool actually works here.
        MACHO_INFO="cryptid=$(otool -l "$BIN" 2>/dev/null |
            /usr/bin/grep -A6 LC_ENCRYPTION_INFO |
            /usr/bin/grep -oE 'cryptid [0-9]+' | head -1 | awk '{print $2}')
gles=$(otool -L "$BIN" 2>/dev/null | /usr/bin/grep -c OpenGLES)"
    fi
fi

case "$MACHO_INFO" in
*"cryptid=1"*)
    echo "WARNING: cryptid 1 -- still FairPlay encrypted." >&2
    echo "  It will install and then fail to launch: the code directory hash" >&2
    echo "  is checked against the first __TEXT page, which is ciphertext." >&2
    echo "  You need a decrypted (Clutch-style) copy." >&2
    WARN=1
    ;;
esac

case "$MACHO_INFO" in
*"gles=0"* | "") ;;
*"gles="*)
    LINKS_GLES=1
    if [ -f "$SHIM" ]; then
        echo "    links OpenGLES -- the GL engine replacement will be installed"
    else
        echo "WARNING: links OpenGLES, and the GL engine replacement is missing." >&2
        echo "  The stock MBXGLEngine drives the PowerVR MBX, which is not" >&2
        echo "  emulated: the app launches, draws its UIKit chrome once, and" >&2
        echo "  then WEDGES THE WHOLE DEVICE spinning on an MBX register that" >&2
        echo "  never acknowledges. Looked for it at:" >&2
        echo "      $SHIM" >&2
        WARN=1
    fi
    ;;
esac

if [ -z "$MACHO_INFO" ] && [ -f "$BIN" ]; then
    echo "NOTE: could not read $EXE's load commands, so the FairPlay and" >&2
    echo "  OpenGLES checks were skipped." >&2
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
# The session file is written by your local ~/Developer/qemu-ios-files/ios3/
# run-ios3.sh --appsync and says which usbmuxd
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

# lockdownd answering ideviceinfo does NOT mean it will start services yet:
# on a freshly-wiped device the first boot brings afcd/installd up well after
# lockdown starts taking queries, and asking too early gets "Could not start
# com.apple.afc: Invalid service" for every service until the guest settles.
# Gate on a real service round trip, not on lockdown liveness.
if ! ideviceinstaller list >/dev/null 2>&1; then
    echo "--- device is up but services are still starting; waiting"
    services_ok=0
    for _ in $(seq 24); do
        sleep 5
        if ideviceinstaller list >/dev/null 2>&1; then
            services_ok=1
            break
        fi
    done
    [ "$services_ok" = 1 ] || echo "install-ipa.sh: services never settled after 120s; trying anyway" >&2
fi

# Say "device is full" while it can still be said clearly: a full device gets
# to ExtractingPackage (15%) and dies with the opaque "PackageExtractionFailed".
# Staging costs roughly the .ipa twice (the copy plus its extraction), so three
# times the archive is a comfortable floor.
AVAIL="$(ideviceinfo -q com.apple.disk_usage -k TotalDataAvailable 2>/dev/null)"
IPA_BYTES="$(stat -f%z "$IPA" 2>/dev/null || echo 0)"
case "$AVAIL" in
'' | *[!0-9]*) ;;   # unreadable -- proceed, installd will have the last word
*)
    if [ "$AVAIL" -lt "$((IPA_BYTES * 3))" ]; then
        die "not enough free space on the device: $((AVAIL / 1048576)) MB free,
    ~$((IPA_BYTES * 3 / 1048576)) MB needed to stage this app. Uninstall something first."
    fi
    ;;
esac

# The placeholder goes up FIRST -- before the GL engine, before the repack,
# before the install -- because it is the only thing on the home screen saying
# anything is happening at all, and everything after it is slow. It used to go
# up after the GL engine replacement, which meant a GL app (the ones people
# actually wait on) spent an iproxy handshake and a whole scp of MBXGLEngine
# showing nothing. Its own upload is one small binary over the tunnel the GL
# copy then reuses, so moving it first costs the install nothing.
#
# Keyed on the bundle id so dropping the same .ipa twice reuses the one icon
# instead of stacking them up.
if [ -n "$BUNDLE_ID" ]; then
    placeholder_add "qemu-install-$BUNDLE_ID" ||
        echo "install-ipa.sh: continuing without the home-screen placeholder" >&2
fi

# The GL engine has to be in place BEFORE the app is launched, and it is worth
# doing before the install too: a wedged device cannot finish an scp, so the
# first launch is the last chance to get anything onto it.
if [ "$LINKS_GLES" = 1 ] && [ -f "$SHIM" ]; then
    echo "--- installing the GL engine replacement"
    # Abort, do not limp on: a GL app installed without the engine drives the
    # real MBX model into unimplemented territory on first launch -- splash
    # screen, then black, then a frozen device. An install that fails loudly
    # here is recoverable by installing again; a wedged device is a reboot.
    install_shim || {
        placeholder_remove
        die "could not install the GL engine (is the ssh tunnel up?); refusing
    to install a GL app without it -- launching one would wedge the device."
    }
fi

echo "--- installing $NAME"
if [ "$NEEDS_MODE" = 1 ]; then
    FIXED="$TMP/$NAME"
    repack_mode "$IPA" "$FIXED" "Payload/$(basename "$APP")/$EXE"
    install_retry "$FIXED"
else
    install_retry "$IPA"
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
