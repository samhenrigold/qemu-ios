#!/bin/bash
#
# Reset Content and Settings.
#
#     imgtools/reset-content.sh <overlay-dir>       # move it aside
#     imgtools/reset-content.sh --list <overlay-dir>  # what has been set aside
#     imgtools/reset-content.sh --undo <overlay-dir>  # put the newest one back
#
# Everything the device has written since it was last clean lives in the
# nandrw= overlay; the base NAND image is never modified. So resetting the
# device is renaming one directory.
#
# It is a RENAME and never a delete, deliberately. The alert on the real thing
# said content would be "moved to the trash", not erased, and that turns out to
# matter more here than it did on a phone: an overlay is the only copy of a
# state that took a long boot and a lot of driving to reach, and the reset is
# one keystroke away from the run you actually wanted to keep.
#
# The emulator must not be running. An overlay renamed out from under a live
# QEMU leaves it writing into an unlinked directory -- the changes go nowhere
# visible and the device looks fine until it is restarted.
set -u

usage() { sed -n '3,25p' "$0" | sed 's/^# \{0,1\}//'; }

MODE=reset
case "${1:-}" in
--list) MODE=list; shift ;;
--undo) MODE=undo; shift ;;
--help | -h)
    usage
    exit 0
    ;;
-*)
    echo "reset-content.sh: unknown flag $1 (try --help)" >&2
    exit 2
    ;;
esac

OVL="${1:-}"
if [ -z "$OVL" ]; then
    usage >&2
    exit 2
fi
OVL="${OVL%/}"
BASE="$(basename "$OVL")"
DIR="$(dirname "$OVL")"

case "$MODE" in
list)
    found=0
    for t in "$DIR/$BASE".trash-*; do
        [ -e "$t" ] || continue
        found=1
        printf '%s  (%s)\n' "$t" "$(du -sh "$t" 2>/dev/null | cut -f1)"
    done
    [ "$found" = 1 ] || echo "nothing set aside for $OVL"
    ;;

undo)
    newest=""
    for t in "$DIR/$BASE".trash-*; do
        [ -e "$t" ] && newest="$t"
    done
    if [ -z "$newest" ]; then
        echo "reset-content.sh: nothing to restore for $OVL" >&2
        exit 1
    fi
    if [ -e "$OVL" ]; then
        # Never overwrite: the current overlay is somebody's state too.
        echo "reset-content.sh: $OVL exists -- reset it first, then --undo" >&2
        exit 1
    fi
    mv "$newest" "$OVL"
    echo "restored $newest -> $OVL"
    ;;

reset)
    if [ ! -d "$OVL" ]; then
        echo "reset-content.sh: no overlay at $OVL (already clean?)" >&2
        exit 1
    fi
    # Refuse while an emulator is using this overlay.
    #
    # lsof is no good here, and it is worth saying why, because it looks like
    # the obvious check and it silently passes: QEMU does not hold the overlay's
    # block files open between writes, so neither `lsof <dir>` nor `lsof +D
    # <dir>` reports anything while a device is happily running on it. This was
    # measured, not assumed -- the first version of this guard let a live
    # emulator's overlay be renamed out from under it.
    #
    # The command line is the reliable signal instead: the overlay only ever
    # reaches QEMU as nandrw=<path> in -M.
    #
    # Split on the comma that separates -M options and on spaces, so the match
    # is an exact whole token: a plain substring test would also fire on
    # nandrw-appsync when asked about nandrw.
    #
    # ps is captured FIRST rather than piped: in a pipeline the grep is itself
    # running, and its own arguments contain the very string it is looking for,
    # so the check reported "an emulator is running" every single time.
    PSOUT="$(ps -Ao args=)"
    if printf '%s\n' "$PSOUT" | tr ', ' '\n\n' | grep -qxF "nandrw=$OVL"; then
        echo "reset-content.sh: an emulator is running on $OVL." >&2
        echo "  Stop it first -- a renamed overlay leaves QEMU writing into an" >&2
        echo "  unlinked directory, which looks fine until the next boot." >&2
        exit 1
    fi
    TRASH="$OVL.trash-$(date +%Y%m%d-%H%M%S)"
    mv "$OVL" "$TRASH"
    mkdir -p "$OVL"
    echo "content and settings reset; the old device is at:"
    echo "  $TRASH"
    echo "put it back with:  $0 --undo $OVL"
    ;;
esac
