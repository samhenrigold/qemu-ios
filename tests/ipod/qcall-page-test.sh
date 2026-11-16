#!/bin/bash
# GATE: models
# QEMU_CALL request blocks across pages on the iPod touch 2G and the iPad.
#
# Guest services read the 52-byte request block and write the reply back through the vCPU's TLB, falling back to a
# debug access for a page that isn't plain RAM there. tests/ipod/qcall-page.S, run as each board's bootrom
# (tests/ipod/bare-rom.sh), issues QC_GLES_PING with the block in one page (the reference), across a page boundary
# whose physical pages are in the opposite order (from SVC and from User mode), with its second page unmapped, and in a
# privileged read-only page (from SVC, and from User mode, which may not access it), and checks each reply is the
# reference's (none for the unmapped one, and no fault; none for User mode in the privileged page).
# QEMU= selects the binary (default build/qemu-system-arm).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECKS="a a block in one page gets a reply
b a block across a page boundary, physical pages out of order, gets the same reply
c the same from User mode
d a block whose second page is unmapped gets none, and doesn't fault
e a block in a read-only page gets the reply, as a debug write gave it
f the same block from User mode, with no User access to the page, gets none"

SRC="$ROOT/tests/ipod/qcall-page.S"
. "$ROOT/tests/ipod/bare-rom.sh"
boards
