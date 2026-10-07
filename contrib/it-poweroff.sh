#!/bin/bash
#
# Shut the guest down cleanly and require its QMP SHUTDOWN confirmation.
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

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
python3 - "$HERE" "$TARGET" "$MAX" <<'PYTHON'
import sys
sys.path.insert(0, sys.argv[1] + "/imgtools")
from itqmp import QMP

q = None
try:
    q = QMP(sys.argv[2], timeout=10)
    try:
        q.cmd("system_powerdown")
    except EOFError:
        pass  # The guest may shut down before the command reply.
    print("it-poweroff: powerdown requested; waiting for guest confirmation", flush=True)
    q.wait_for_guest_shutdown(float(sys.argv[3]))
    print("it-poweroff: guest-confirmed SHUTDOWN")
except (OSError, EOFError, RuntimeError, ValueError) as exc:
    sys.exit("it-poweroff: shutdown not confirmed: %s" % exc)
finally:
    if q is not None:
        q.close()
PYTHON
