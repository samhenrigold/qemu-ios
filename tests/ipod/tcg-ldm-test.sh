#!/bin/bash
# LDM, STM, PUSH and POP moved two or four words per access on the iPad (Cortex-A8).
#
# TCG moves the words of a block transfer as one 64- or 128-bit access (MO_SPLIT_UL), so its fast path checks the
# TLB once for them, and its slow path moves them one word at a time as before. tests/ipod/tcg-ldm.S, run as the
# iPad's bootrom (tests/ipod/bare-rom.sh), compares block transfers with single loads and stores, and checks what
# a fault, a device or rewritten code makes of them. QEMU= selects the binary (default build/qemu-system-arm).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECKS="a A32 STM in the four addressing modes, lists of 8, 3, 6 and 2 registers, as single stores
b A32 LDM in the four addressing modes, lists of 8, 3 and 6, as single loads
c Thumb push and pop with LR and PC through a call, T16 and T32 LDM and STM, LDM and STM in IT blocks
d LDM with the base in the list (A32 and T16) and STM storing the base it writes back
f a data abort at the first word of an unmapped page part way through LDM (with the PC too): DFAR, DFSR, no writeback
g a data abort part way through STM: the words before the page stored, as single stores do; DFAR, DFSR, no writeback
h an alignment fault at a misaligned base, LDM and STM of 2 and 4 words, nothing stored
i STM and LDM of 4 and 2 words on device registers that take only 4-byte accesses
k a function rewritten by STM of 2 and 4 words runs its new code
l a conditional pop {..pc} ending an IT block, taken and not
m STM^ and LDM^ move the user-mode SP and LR"

SRC="$ROOT/tests/ipod/tcg-ldm.S"
. "$ROOT/tests/ipod/bare-rom.sh"
board Cortex-A8 ipad1 0x40000000 65536 ""
exit $fail
