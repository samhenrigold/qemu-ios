#!/bin/bash
# ARM1176 DSCR: a user-mode BKPT's prefetch abort can read why it happened.
#
# iPhone OS 2.x's prefetch-abort handler reads the v6 DSCR (MRC p14, 0, Rt, c0, c1, 0) to tell a BKPT (SIGTRAP for
# the process) from a fault. Without the register the read UNDEFs in the kernel and the iPod panics ("undefined
# kernel instruction"); 2.2's it_prefs hit it through a CoreFoundation HALT.
#
# A bare-metal program on the built emulator (versatilepb, -cpu arm1176), loaded and entered at 0x10000 with the MMU
# off and vectors at 0, drops to user mode and runs BKPT; its prefetch-abort handler reads DSCR and prints P when MOE
# (bits 5:2) is 3 (BKPT), M for any other MOE, and its undefined-instruction handler prints U. QEMU= selects the
# binary (default build/qemu-system-arm).
set -u
QEMU="${QEMU:-$(cd "$(dirname "$0")/../.." && pwd)/build/qemu-system-arm}"
[ -x "$QEMU" ] || { echo "FAIL no emulator at $QEMU: build it or set QEMU="; exit 1; }
TMP="$(mktemp -d "${TMPDIR:-/tmp}/dscr.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT
code=(
    e3a00000 e59f1054 e5801004 e580100c   # mov r0,#0; ldr r1,=ldr pc,[pc,#0x18]; undef and prefetch-abort vectors
    e28f1038 e5801024 e28f100c e580102c   # their targets: the undef handler (0x50), the prefetch-abort handler (0x2c)
    e321f010 e1200070 eafffffe            # msr cpsr_c,#0x10 (user); bkpt #0; b .
    ee101e11 e1a01121 e201100f e3510003   # mrc p14,0,r1,c0,c1,0 (DSCR); MOE = (r1 >> 2) & 15; cmp #3
    03a02050 13a0204d e59f3018 e5832000 eafffffe   # 'P' if BKPT else 'M' to UART0; b .
    e3a02055 e59f3008 e5832000 eafffffe   # undef handler: 'U' to UART0; b .
    e59ff018 101f1000                     # literals: ldr pc,[pc,#0x18]; versatilepb PL011 UART0
)
for word in "${code[@]}"; do printf '%s' "${word:6:2}${word:4:2}${word:2:2}${word:0:2}"; done | xxd -r -p > "$TMP/bkpt.bin"
"$QEMU" -M versatilepb -cpu arm1176 -m 16M -display none -monitor none -serial "file:$TMP/serial" \
    -audio driver=none -kernel "$TMP/bkpt.bin" </dev/null >/dev/null 2>&1 &
pid=$!
for _ in $(seq 100); do [ -s "$TMP/serial" ] && break; sleep 0.2; done
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
case "$(head -c 1 "$TMP/serial" 2>/dev/null)" in
    P) echo "PASS DSCR.MOE = BKPT after a user-mode BKPT" ;;
    M) echo "FAIL DSCR readable but MOE is not BKPT (3)"; exit 1 ;;
    U) echo "FAIL DSCR read is UNDEFINED (2.x kernel: undefined kernel instruction panic)"; exit 1 ;;
    *) echo "FAIL no verdict from the guest"; exit 1 ;;
esac
