#!/bin/bash
# GATE: models
# CPSR writes on the ARM1176 and Cortex-A8 boards: interrupt unmasking, masking inside a block, flag writes, modes.
#
# A write that only changes the flags or the A/I/F masks rebuilds no translation flags, and a cpsid keeps its block
# going; everything else must still behave as on hardware. tests/ipod/tcg-psr.S, run as the bootrom of the iPod touch
# 2G and the iPad (tests/ipod/bare-rom.sh), checks that an IRQ raised through VIC0 while masked is taken right after
# the instruction that unmasks it (cpsie i, msr cpsr_c, msr cpsr_fc) and a FIQ right after cpsie f, that cpsid i
# inside a block keeps a later IRQ pending across a block boundary, that msr cpsr_fc and VMRS APSR_nzcv set every
# flag, and that cps and msr cpsr_c mode changes reach the banked sp and the privilege level (a CP15 access in User
# mode is undefined). QEMU= selects the binary (default build/qemu-system-arm).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECKS="a an IRQ pending while masked is taken right after cpsie i
b the same after msr cpsr_c
cd the same after msr cpsr_fc, and its flags land
ef cpsid i inside a block keeps a later IRQ pending until cpsie i, which takes it at once
g a FIQ pending while masked is taken right after cpsie f
hij VMRS APSR_nzcv after VCMP: less, equal, unordered
kl cps to System and back reaches each mode's banked sp
m the same through msr cpsr_c
nop msr cpsr_c to User: a CP15 access is undefined there, svc returns to SVC"

SRC="$ROOT/tests/ipod/tcg-psr.S"
. "$ROOT/tests/ipod/bare-rom.sh"
boards
