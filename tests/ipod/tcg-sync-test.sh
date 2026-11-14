#!/bin/bash
# What an exception, a rewound device access and a TLB miss see of the guest's registers on the iPad (Cortex-A8).
#
# TCG keeps the guest registers and flags in host registers across a memory access and stores them only on the
# access's slow path (TCG_TARGET_LDST_LIVE), which also keeps the host registers a helper call would clobber.
# tests/ipod/tcg-sync.S, run as the iPad's bootrom (tests/ipod/bare-rom.sh), checks the registers and flags a data
# abort records, sums across loads that miss the TLB, and loops around device accesses part way through a block.
# QEMU= selects the binary (default build/qemu-system-arm).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECKS="a registers and flags set just before a load or store that aborts, used or overwritten after it
b a load that aborts into its own base or a register set just before it, and a post-indexed one, change neither
c Thumb: registers and flags set before a load that aborts and overwritten after it
d twelve registers live across loads that miss the TLB
f a device read part way through a block, between updates of a register and the flags
g a device store and load part way through a block, with flags set before them and used after"

SRC="$ROOT/tests/ipod/tcg-sync.S"
. "$ROOT/tests/ipod/bare-rom.sh"
board Cortex-A8 ipad1 0x40000000 65536 ""
exit $fail
