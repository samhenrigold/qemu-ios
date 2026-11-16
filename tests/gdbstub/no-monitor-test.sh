#!/bin/bash
# GATE: models
# The gdbstub carries no monitor (gdbstub/system.c): a debugger on the stub gets the guest, and a `monitor` packet
# (qRcmd) gets the empty reply of an unsupported packet instead of this process's HMP monitor. Runs the iPad's
# machine paused (-S) on a blank bootrom with the stub on a Unix socket, as LightTouchMac starts it, and checks that
# the stub answers `?` and refuses qRcmd for `info version`. QEMU= selects the binary (default build/qemu-system-arm).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
[ -x "$QEMU" ] || { echo "FAIL no emulator at $QEMU: build it or set QEMU="; exit 1; }
TMP="$(mktemp -d "${TMPDIR:-/tmp}/gdbstub.XXXXXX")"
truncate -s 65536 "$TMP/rom"
"$QEMU" -M "ipad1,bootrom=$TMP/rom" -S -gdb "unix:$TMP/g.sock,server=on,wait=off" -display none -monitor none \
    -serial none -nic none -audio driver=none </dev/null >"$TMP/qemu.log" 2>&1 &
pid=$!
trap 'kill $pid 2>/dev/null; wait $pid 2>/dev/null; rm -rf "$TMP"' EXIT
python3 -I - "$TMP/g.sock" <<'PY'
import socket, sys, time

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

for _ in range(100):
    try:
        s = socket.socket(socket.AF_UNIX)
        s.connect(sys.argv[1])
        break
    except OSError:
        time.sleep(0.1)
else:
    print('FAIL no gdbstub at the socket'); sys.exit(1)
s.settimeout(10)
fail = 0
stop = packet(s, b'?')
ok = stop is not None and stop[:1] in (b'S', b'T')
print('%s the stub answers ? (%r)' % ('PASS' if ok else 'FAIL', stop)); fail |= not ok
reply = packet(s, b'qRcmd,' + b'info version'.hex().encode())
ok = reply == b''
print('%s qRcmd is unsupported: no monitor behind the stub (%r)' % ('PASS' if ok else 'FAIL', reply)); fail |= not ok
sys.exit(fail)
PY
