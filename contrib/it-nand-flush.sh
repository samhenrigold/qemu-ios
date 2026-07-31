#!/bin/bash
#
# Flush the guest's filesystem to the NAND overlay before stopping the emulator.
#
#     contrib/it-nand-flush.sh <overlay-dir> [udid]
#
# Why this is needed: the guest writes file *data* to flash promptly, but HFS+
# holds catalog updates in memory. Nothing forces them out on an idle device --
# measured, no page reaches flash in the three minutes after an `afcclient put`
# -- so killing QEMU loses the directory entry even though every data block is
# safely on disk. The file then does not exist on the next boot.
#
# There is no plain "reboot" in iOS 2.1.1's lockdownd and no diagnostics relay,
# but asking it to enter recovery makes it tear the filesystem down on the way
# out, and that flush is what we are after. It reports "Failed to enter recovery
# mode" because the machine has no working reset path; that is expected and does
# not mean the flush failed. The device is unusable afterwards, so call this
# last, then stop QEMU.
set -u

OVL="${1:?usage: it-nand-flush.sh <overlay-dir> [udid]}"
UDID="${2:-$(idevice_id -l 2>/dev/null | head -1)}"
[ -n "$UDID" ] || { echo "it-nand-flush: no device"; exit 1; }

pages() { ls "$OVL"/cs* 2>/dev/null | grep -c '\.page$'; }

before=$(pages)
ideviceenterrecovery "$UDID" >/dev/null 2>&1

# Wait for the shutdown write burst to finish rather than guessing a delay.
last=-1
for _ in $(seq 1 30); do
    sleep 2
    now=$(pages)
    [ "$now" = "$last" ] && [ "$now" != "$before" ] && break
    last=$now
done

after=$(pages)
echo "it-nand-flush: $before -> $after pages in $OVL"
[ "$after" -gt "$before" ] || {
    echo "it-nand-flush: nothing was flushed - was the device still up?" >&2
    exit 1
}
