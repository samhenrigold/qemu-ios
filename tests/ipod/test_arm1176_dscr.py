#!/usr/bin/env python3
"""ARM1176 DSCR: a user-mode BKPT's prefetch abort can read why it happened.

iPhone OS 2.x's prefetch-abort handler reads the v6 DSCR (MRC p14, 0, Rt, c0, c1, 0) to tell a BKPT
(SIGTRAP for the process) from a fault. Without the register the read UNDEFs in the kernel and the
iPod panics ("undefined kernel instruction"); 2.2's it_prefs hit it through a CoreFoundation HALT.

A bare-metal program on the built emulator (versatilepb, -cpu arm1176) drops to user mode and runs
BKPT; its prefetch-abort handler reads DSCR and prints P when MOE (bits 5:2) is 3 (BKPT), M for any
other MOE, and its undefined-instruction handler prints U. QEMU= selects the binary.
"""
import os, select, struct, subprocess, sys, tempfile
from pathlib import Path

QEMU = os.environ.get("QEMU", str(Path(__file__).resolve().parents[2] / "build/qemu-system-arm"))
CODE = [                      # loaded and entered at 0x10000, MMU off, vectors at 0
    0xe3a00000,  # 00 mov  r0, #0
    0xe59f1054,  # 04 ldr  r1, [pc, #0x54]      ; ldr pc, [pc, #0x18]
    0xe5801004,  # 08 str  r1, [r0, #4]         ; undef vector -> word at 0x24
    0xe580100c,  # 0c str  r1, [r0, #0xc]       ; prefetch-abort vector -> word at 0x2c
    0xe28f1038,  # 10 add  r1, pc, #0x38        ; undef handler (0x50)
    0xe5801024,  # 14 str  r1, [r0, #0x24]
    0xe28f100c,  # 18 add  r1, pc, #0xc         ; prefetch-abort handler (0x2c)
    0xe580102c,  # 1c str  r1, [r0, #0x2c]
    0xe321f010,  # 20 msr  cpsr_c, #0x10        ; user mode
    0xe1200070,  # 24 bkpt #0
    0xeafffffe,  # 28 b    .
    0xee101e11,  # 2c mrc  p14, 0, r1, c0, c1, 0 ; DSCR
    0xe1a01121,  # 30 lsr  r1, r1, #2
    0xe201100f,  # 34 and  r1, r1, #15
    0xe3510003,  # 38 cmp  r1, #3
    0x03a02050,  # 3c moveq r2, #'P'
    0x13a0204d,  # 40 movne r2, #'M'
    0xe59f3018,  # 44 ldr  r3, [pc, #0x18]      ; UART0
    0xe5832000,  # 48 str  r2, [r3]
    0xeafffffe,  # 4c b    .
    0xe3a02055,  # 50 mov  r2, #'U'
    0xe59f3008,  # 54 ldr  r3, [pc, #8]
    0xe5832000,  # 58 str  r2, [r3]
    0xeafffffe,  # 5c b    .
    0xe59ff018,  # 60 (literal) ldr pc, [pc, #0x18]
    0x101f1000,  # 64 (literal) versatilepb PL011 UART0
]

if not os.access(QEMU, os.X_OK):
    sys.exit(f"FAIL no emulator at {QEMU}: build it or set QEMU=")
with tempfile.TemporaryDirectory() as tmp:
    image = Path(tmp) / "bkpt.bin"
    image.write_bytes(struct.pack(f"<{len(CODE)}I", *CODE))
    p = subprocess.Popen([QEMU, "-M", "versatilepb", "-cpu", "arm1176", "-m", "16M", "-display", "none",
                          "-monitor", "none", "-serial", "stdio", "-audio", "driver=none", "-kernel", str(image)],
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    try:
        out = p.stdout.read(1) if select.select([p.stdout], [], [], 20)[0] else b""
    finally:
        p.kill()
        p.wait()
verdict = {b"P": "PASS DSCR.MOE = BKPT after a user-mode BKPT",
           b"M": "FAIL DSCR readable but MOE is not BKPT (3)",
           b"U": "FAIL DSCR read is UNDEFINED (2.x kernel: undefined kernel instruction panic)"}
print(verdict.get(out, f"FAIL no verdict from the guest ({out!r})"))
sys.exit(0 if out == b"P" else 1)
