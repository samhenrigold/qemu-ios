#!/bin/bash
# GATE: models
# Branches that leave their TB through TCG's jump cache probe, on the iPad (Cortex-A8).
#
# A call, a return or an indirect branch finds its next TB in the jump cache from generated code, as
# helper_lookup_tb_ptr would, trusting that nothing but the PC, the Thumb bit and the IT bits changed since the TB
# began. tests/ipod/tcg-probe.S, run as the iPad's bootrom (tests/ipod/bare-rom.sh), checks calls and returns across
# pages, interworking in both directions, a conditional return in an IT block, and an indirect call and a return into
# a page remapped (TLBIMVA) after the probe had found its code, code at one address entered as Thumb and then
# as ARM, and one physical page of code that reads the PC, run at two virtual addresses (one TB each, or one
# PC-relative TB). QEMU= selects the binary (default
# build/qemu-system-arm).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECKS="a a call to another page and its return, 1000 times
b Thumb to ARM and back (blx imm, bx lr) and ARM to Thumb and back (blx reg, pop {pc})
c a conditional return in an IT block
d an indirect call into a page remapped after 100 calls
e a return into a page remapped by the call it returns from
f one address run as Thumb, then as ARM: the probe keys on the Thumb bit
g one physical page at two virtual addresses: its code reads the PC it runs at
h three return addresses in one jump cache set, in turn: each returns to its own code"

SRC="$ROOT/tests/ipod/tcg-probe.S"
. "$ROOT/tests/ipod/bare-rom.sh"
board Cortex-A8 ipad1 0x40000000 65536 ""
exit $fail
