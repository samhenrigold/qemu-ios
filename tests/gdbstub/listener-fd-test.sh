#!/bin/bash
# GATE: models
# The gdbstub on a listening socket the caller made and passed (-chardev socket,fd=N,server=on -gdb chardev:ID), as
# LightTouchMac starts a sandboxed helper's: the caller binds the path somewhere the helper can't reach and QEMU
# only accepts on the descriptor. Runs the iPad's machine paused (-S) on a blank bootrom and checks that a debugger
# connecting to the caller's path gets the stub's answer to `?`. QEMU= selects the binary (default
# build/qemu-system-arm).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
[ -x "$QEMU" ] || { echo "FAIL no emulator at $QEMU: build it or set QEMU="; exit 1; }
TMP="$(mktemp -d "${TMPDIR:-/tmp}/gdbfd.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
truncate -s 65536 "$TMP/rom"
python3 -I - "$QEMU" "$TMP" <<'PY'
import socket, subprocess, sys, time

qemu, tmp = sys.argv[1], sys.argv[2]
path = tmp + "/g.sock"
listener = socket.socket(socket.AF_UNIX)
listener.bind(path)
listener.listen(1)
fd = listener.fileno()
p = subprocess.Popen([qemu, "-M", "ipad1,bootrom=" + tmp + "/rom", "-S", "-display", "none", "-monitor", "none",
                      "-serial", "none", "-nic", "none", "-audio", "driver=none",
                      "-chardev", f"socket,id=gdb0,fd={fd},server=on,wait=off", "-gdb", "chardev:gdb0"],
                     pass_fds=(fd,), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
listener.close()  # QEMU holds the listener now

def packet(s, body):
    s.sendall(b'$%s#%02x' % (body, sum(body) & 0xff))
    data = b''
    while True:
        chunk = s.recv(4096)
        if not chunk:
            return None
        data += chunk
        start = data.find(b'$')
        end = data.find(b'#', start)
        if start >= 0 and end >= 0 and len(data) >= end + 3:
            s.sendall(b'+')
            return data[start + 1:end]

try:
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(10)
    for _ in range(100):
        try:
            s.connect(path)
            break
        except OSError:
            time.sleep(0.1)
    stop = packet(s, b'?')
    ok = stop is not None and stop[:1] in (b'S', b'T')
    if p.poll() is not None:
        print('FAIL QEMU exited: ' + p.stdout.read().decode(errors='replace')); sys.exit(1)
    print('%s the stub answers ? on the caller\'s listener (%r)' % ('PASS' if ok else 'FAIL', stop))
    sys.exit(0 if ok else 1)
finally:
    p.kill(); p.wait()
PY
