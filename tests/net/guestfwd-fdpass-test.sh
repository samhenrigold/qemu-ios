#!/bin/bash
# GATE: models
# The user netdev's guestfwd fdpass:FD as the command line takes it: QEMU starts with FD a Unix stream socket it
# inherited (and holds it open), and refuses a descriptor that is a pipe, a datagram socket or not open, naming the
# rule. Each connection's handoff is tests/slice/net-slirp-fdpass.c. QEMU= selects the binary (default
# build/qemu-system-arm).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
[ -x "$QEMU" ] || { echo "FAIL no emulator at $QEMU: build it or set QEMU="; exit 1; }
python3 -I - "$QEMU" <<'PY'
import os, socket, subprocess, sys, time

qemu = sys.argv[1]

def start(fd, keep):
    rule = f"user,id=n0,guestfwd=tcp:10.0.2.100:3128-fdpass:{fd}"
    return subprocess.Popen([qemu, "-M", "none", "-S", "-display", "none", "-monitor", "none", "-serial", "none",
                             "-netdev", rule], pass_fds=keep, stdin=subprocess.DEVNULL,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)

ours, theirs = socket.socketpair(socket.AF_UNIX, socket.SOCK_STREAM)
p = start(theirs.fileno(), (theirs.fileno(),))
time.sleep(1.5)
if p.poll() is not None:
    sys.exit("FAIL a Unix stream socket refused: " + p.stdout.read().decode(errors="replace"))
p.kill(); p.wait()

r, w = os.pipe()
dgram = socket.socket(socket.AF_UNIX, socket.SOCK_DGRAM)
for name, fd, keep in [("a pipe", r, (r,)), ("a datagram socket", dgram.fileno(), (dgram.fileno(),)),
                       ("a descriptor not open", 97, ())]:
    p = start(fd, keep)
    try:
        out, _ = p.communicate(timeout=10)
    except subprocess.TimeoutExpired:
        p.kill(); sys.exit(f"FAIL {name} accepted")
    text = out.decode(errors="replace")
    if p.returncode == 0 or "fdpass: needs a Unix stream socket" not in text:
        sys.exit(f"FAIL {name}: exit {p.returncode}: {text}")
print("PASS fdpass: takes a Unix stream socket and refuses a pipe, a datagram socket and a closed descriptor")
PY
