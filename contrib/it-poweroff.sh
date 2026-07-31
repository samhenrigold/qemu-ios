#!/bin/bash
#
# Shut the guest down cleanly and wait for QEMU to stop by itself.
#
#     contrib/it-poweroff.sh <qmp-socket-or-host:port> [max-seconds]
#
# Requires the emulator to have been started with a QMP monitor, e.g.
#     -qmp unix:/tmp/it.sock,server,nowait
#
# This issues QEMU's system_powerdown, which the iPod Touch machine turns into
# the "slide to power off" gesture (hold button, then a drag across the slider
# SpringBoard raises). iPhone OS 2.1.1 offers no remote shutdown -- lockdownd
# has no reboot request and no diagnostics relay, and SpringBoard runs as
# `mobile`, so nothing in the guest can call reboot(2) -- so the gesture is the
# only route to a clean shutdown.
#
# Why bother: the guest writes file *data* to flash promptly but keeps HFS+
# catalog updates in memory, so a file written over AFC and then SIGKILLed away
# has all its blocks on disk and no directory entry, and does not exist on the
# next boot. Unmounting the root volume is what flushes the catalog, and only a
# real shutdown unmounts it.
#
# When the guest is done it clears the PMU's power latch (register 0x10 bit 6);
# the PMU model turns that into a shutdown request, so QEMU exits on its own and
# the overlay is complete. This replaces contrib/it-nand-flush.sh.
set -u

TARGET="${1:?usage: it-poweroff.sh <qmp-socket-or-host:port> [max-seconds]}"
MAX="${2:-90}"

python3 - "$TARGET" <<'EOF'
import json, socket, sys

target = sys.argv[1]
if ':' in target and not target.startswith('/'):
    host, port = target.rsplit(':', 1)
    s = socket.create_connection((host, int(port)))
else:
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.connect(target)

f = s.makefile('rwb')
f.readline()                                     # greeting
f.write(b'{"execute":"qmp_capabilities"}\n'); f.flush(); f.readline()
f.write(b'{"execute":"system_powerdown"}\n'); f.flush()
while True:
    line = f.readline()
    if not line:
        break
    if 'event' not in json.loads(line):
        break
EOF

echo "it-poweroff: powerdown requested; the guest takes ~10s to unmount and halt"

# The QMP socket disappearing is QEMU exiting.
i=0
while [ "$i" -lt "$MAX" ]; do
    if ! python3 -c "
import socket,sys
t='$TARGET'
try:
    if ':' in t and not t.startswith('/'):
        h,p=t.rsplit(':',1); socket.create_connection((h,int(p)),1).close()
    else:
        s=socket.socket(socket.AF_UNIX,socket.SOCK_STREAM); s.connect(t); s.close()
except Exception:
    sys.exit(1)
" 2>/dev/null; then
        echo "it-poweroff: QEMU has stopped after ${i}s"
        exit 0
    fi
    sleep 2
    i=$((i + 2))
done

echo "it-poweroff: QEMU still running after ${MAX}s" >&2
exit 1
