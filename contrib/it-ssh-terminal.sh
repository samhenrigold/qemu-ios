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
# the usbmuxd that run-ios3.sh starts BEFORE QEMU. Without it there is no
# transport and no amount of retrying helps.
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
SOCK="${USBMUXD_SOCKET_ADDRESS:-}"
if [ -z "$SOCK" ] && [ -f "$HOME/Developer/qemu-ios-files/apps/work/session.env" ]; then
    # shellcheck disable=SC1091
    . "$HOME/Developer/qemu-ios-files/apps/work/session.env"
    SOCK="${SOCK:-}"
fi
[ -n "$SOCK" ] && export USBMUXD_SOCKET_ADDRESS="$SOCK"

if ! USBMUXD_SOCKET_ADDRESS="${SOCK:-}" idevice_id -l >/dev/null 2>&1; then
    echo "no device on usbmuxd${SOCK:+ at $SOCK} -- start the emulator with --appsync first" >&2
    exit 1
fi

# The window's whole life in one script: bring the tunnel up, hand the user a
# shell, take the tunnel down when they leave. iproxy is not left running,
# because a second one on the same port fails silently and the NEXT window is
# the one that appears broken.
CMD="$(mktemp -t itssh).command"
cat >"$CMD" <<EOF
#!/bin/bash
cleanup() { [ -n "\${IPROXY:-}" ] && kill "\$IPROXY" 2>/dev/null; rm -f "$CMD"; }
trap cleanup EXIT
${SOCK:+export USBMUXD_SOCKET_ADDRESS=$SOCK}
iproxy $PORT 22 >/dev/null 2>&1 &
IPROXY=\$!
sleep 1
kill -0 "\$IPROXY" 2>/dev/null || { echo "iproxy would not start on port $PORT."; read -r; exit 1; }
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
