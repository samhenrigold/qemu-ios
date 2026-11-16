#!/bin/bash
# GATE: models
# The Cortex-A8's PMU with its EL-change hooks run without the BQL.
#
# The hooks bring the counters up to date at every exception entry and return; they now run on the vCPU without the
# BQL, and the counters' overflow timer hands its update to the vCPU (async_run_on_cpu) instead of making it from the
# main loop. tests/ipod/tcg-pmu.S, run as the iPad's bootrom (tests/ipod/bare-rom.sh), checks that the cycle counter
# grows across a thousand svc and movs pc, lr round trips and that an overflow 4096 cycles out reaches PMOVSR while
# the program only polls it. QEMU= selects the binary (default build/qemu-system-arm).
set -u
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
CHECKS="a the cycle counter grows across 1000 exceptions taken and returned
b a cycle counter overflow reaches PMOVSR through the counter's timer"

SRC="$ROOT/tests/ipod/tcg-pmu.S"
. "$ROOT/tests/ipod/bare-rom.sh"
board Cortex-A8 ipad1 0x40000000 65536 ""
exit $fail
