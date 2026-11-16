#!/bin/bash
# GATE: models
# ASID switches, TLB maintenance and rewritten code on the ARM1176 and Cortex-A8 boards.
#
# TCG keeps a TB jump cache per ASID, the kernel's global TLB entries across an ASID switch, and the user (PL0)
# entries of the ASID last left, as the hardware's tagged TLB does. Both are only right if every way a guest changes or invalidates a mapping still reaches them:
# non-global kernel-mode pages must go at a switch (one by one, or all past 32), a page invalidated while another ASID
# runs must leave that ASID's jump cache, as must a TLBIASID or TLBIALL, a reused cache must start empty, and code
# rewritten through another mapping (a JIT) must be seen in every address space; kept user entries must go with any
# invalidation that would take them from a tagged TLB, and writes through them must still find translated code.
#
# tests/ipod/tcg-asid.S does each of these on the iPod touch 2G (ARM1176, RAM at 0x08000000) and the iPad
# (Cortex-A8, RAM at 0x40000000), run as the bootrom (tests/ipod/bare-rom.sh); both boards use 4K target pages, as
# the kept-entry path needs. QEMU= selects the binary (default build/qemu-system-arm).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECKS="abc kernel load from a non-global page follows the ASID switch
de more non-global kernel pages than are tracked one by one
fgh code at one VA in two ASIDs, reached by an indirect branch
i ASID 1's code remapped while ASID 2 runs, TLBIMVA
j the same, TLBIASID
kl the current ASID's code remapped, TLBIASID, then TLBIMVA
m more ASIDs than jump caches
nopq a global mapping kept across switches, then remapped (TLBIMVA, TLBIALL)
rst code rewritten through another mapping, in another ASID and in its own
u ASID 1's code remapped while ASID 2 runs, TLBIALL
v user (PL0) pages read unprivileged in two ASIDs
w a user entry kept across a round trip to another ASID (remapped without TLBI: still the old page)
x a kept user entry invalidated while another ASID runs, TLBIMVA
y the same, TLBIASID
z the same, TLBIALL
1 code written unprivileged through a kept user entry is seen
2 a TLBIMVA anywhere in a 1M section takes every page of it
3 a TLBIMVA in one section leaves another's entries (remapped without TLBI: still the old page)
4 a non-global section the kernel read goes at an ASID switch, every page of it
5 and only it: a global section's entry stays (remapped without TLBI: still the old page)"

SRC="$ROOT/tests/ipod/tcg-asid.S"
. "$ROOT/tests/ipod/bare-rom.sh"
boards
