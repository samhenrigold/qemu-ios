#!/bin/bash
#
# Open a Terminal window with a root shell on the emulated device.
#
#     contrib/it-ssh-terminal.sh [port]
#
# Also the Device ▸ Open Terminal menu item. There is nothing here you could not
# type yourself -- it is `iproxy` plus `ssh` -- but the two have to be ordered
# and torn down together, and the failure mode when they are not is an ssh that
# hangs for a minute and then says nothing useful.
#
# REQUIRES a --appsync (or --usb) run: ssh reaches the guest over USB, through
# the usbmuxd that contrib/run-ipod-touch.sh (or your local
# ~/Developer/qemu-ios-files/ios3/run-ios3.sh) starts BEFORE QEMU. Without it
# there is no transport and no amount of retrying helps.
set -u

PORT="${1:-${SSH_PORT:-2239}}"
PASSWORD="${DEVICE_PASSWORD:-alpine}"

command -v iproxy >/dev/null || {
    echo "no iproxy on PATH -- install libimobiledevice (brew install libimobiledevice)" >&2
    exit 1
}

# usbmuxd has to be listening before iproxy will connect to anything. Checking
# here turns "the terminal opened and ssh timed out" into a sentence that names
# the actual problem.
SESSION="${SESSION:-${IPOD_FILES:-$HOME/Developer/qemu-ios-files}/apps/work/session.env}"
SOCK="${USBMUXD_SOCKET_ADDRESS:-}"
if [ -z "$SOCK" ] && [ -f "$SESSION" ]; then
    # shellcheck disable=SC1090
    . "$SESSION"
    SOCK="${SOCK:-}"
fi

# No socket means no emulator of OURS. Falling through to the default would
# reach the Mac's own Apple usbmuxd, which is running, answers happily, and has
# no emulated device on it -- so iproxy connects to nothing and ssh dies
# instantly with no message. Refuse instead of producing that.
if [ -z "$SOCK" ]; then
    echo "no emulator session at $SESSION -- start the emulator with --appsync" >&2
    echo "(the packaged app passes --appsync for you)" >&2
    exit 1
fi
export USBMUXD_SOCKET_ADDRESS="$SOCK"

# idevice_id exits 0 with an EMPTY list when usbmuxd is up but has no device, so
# the exit status alone cannot tell "connected" from "nothing there".
if [ -z "$(idevice_id -l 2>/dev/null)" ]; then
    echo "usbmuxd at $SOCK has no device attached yet." >&2
    echo "Wait for the device to finish booting, then try again." >&2
    exit 1
fi

# The window's whole life in one script: bring the tunnel up, hand the user a
# shell, take the tunnel down when they leave. iproxy is not left running,
# because a second one on the same port fails silently and the NEXT window is
# the one that appears broken.
# Terminal runs this under the USER'S LOGIN SHELL, not ours, so it does not
# inherit the PATH the app launcher set -- a bare `iproxy` here is
# command-not-found on any Mac without Homebrew even though the bundled one is
# sitting right there. Resolve it to an absolute path now, while we still have
# the PATH that can find it.
IPROXY_BIN="$(command -v iproxy)"
# mktemp CREATES the file it names; appending .command means the script is
# written somewhere else and the original is leaked on every use. Make the
# directory instead and put the script inside it.
CMD="$(mktemp -d -t itssh)/iPod touch.command"
cat >"$CMD" <<EOF
#!/bin/bash
# disown first: otherwise job control prints "Terminated: 15" over the last
# thing the user was reading, which looks like the shell crashed.
cleanup() { if [ -n "\${IPROXY:-}" ]; then disown "\$IPROXY" 2>/dev/null; kill "\$IPROXY" 2>/dev/null; fi; rm -rf "$(dirname "$CMD")"; }
trap cleanup EXIT
export USBMUXD_SOCKET_ADDRESS=$SOCK
"$IPROXY_BIN" $PORT 22 >/dev/null 2>&1 &
IPROXY=\$!
sleep 1
kill -0 "\$IPROXY" 2>/dev/null || { echo "The USB tunnel would not start on port $PORT -- something else may be using it."; read -r; exit 1; }
echo "iPod touch 2G -- iOS 3.1.3.  The root password is: $PASSWORD"
echo
ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \\
    -o LogLevel=ERROR -o ConnectTimeout=10 \\
    -p $PORT root@127.0.0.1
echo
echo "[connection closed -- you can close this window]"
EOF
chmod 755 "$CMD"
open -a Terminal "$CMD"
