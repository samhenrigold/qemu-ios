#!/bin/bash
# The iPhone 5's translations run on 4K target pages.
#
# Every CPU the A6 machine makes (the application cores, the IOP and the AE2) is a v7 VMSA core, which can't map a
# page under 4K (VMSAv7 has no tiny pages), so a block may run on past a 1K boundary and a branch within 4K chains.
# A bare-metal program as the machine's kboot image (loaded and entered at the start of DRAM, MMU off) runs sixteen
# NOPs across 0x80000400 and exits through semihosting; under -d exec,nochain, no block may start at 0x80000400 (the
# boundary of a 1K page). QEMU= selects the binary (default build/qemu-system-arm).
set -u
QEMU="${QEMU:-$(cd "$(dirname "$0")/../.." && pwd)/build/qemu-system-arm}"
[ -x "$QEMU" ] || { echo "FAIL no emulator at $QEMU: build it or set QEMU="; exit 1; }
TMP="$(mktemp -d "${TMPDIR:-/tmp}/tcga6.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

cat > "$TMP/p.S" <<'ASM'
        .syntax unified
        .arch armv7-a
        .text
        .arm
_start: b       go
        .space  0x3e0 - 4
go:     .rept   16
        nop
        .endr
        mov     r0, #0x18           @ SYS_EXIT
        ldr     r1, =0x20026        @ ADP_Stopped_ApplicationExit
        svc     0x123456
        b       .
        .ltorg
ASM
clang --target=armv7a-none-eabi -c "$TMP/p.S" -o "$TMP/p.o" || { echo "FAIL couldn't build the program"; exit 1; }
# The image, padded to 4K, then the trailer the machine reads: K48KBOOT, load, entry, boot arguments, length.
python3 -I - "$TMP/p.o" "$TMP/kboot" <<'PY' || { echo "FAIL couldn't build the kboot image"; exit 1; }
import struct, sys
d = open(sys.argv[1], 'rb').read()
shoff, = struct.unpack_from('<I', d, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2e)
secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
name = lambda s: d[secs[shstrndx][4] + s[0]:].split(b'\0', 1)[0]
t = next(s for s in secs if name(s) == b'.text')
text = d[t[4]:t[4] + t[5]].ljust(0x1000, b'\0')
dram = 0x80000000
open(sys.argv[2], 'wb').write(text + b'K48KBOOT' + struct.pack('<IIII', dram, dram, dram + 0x800, len(text)))
PY
${TIMEOUT:+$TIMEOUT 60} "$QEMU" -M "iPhone-5,kboot=$TMP/kboot" -display none -nodefaults -monitor none -serial none \
    -audio driver=none -semihosting-config enable=on,target=native -d exec,nochain -D "$TMP/exec.log" \
    </dev/null >/dev/null 2>&1
if ! grep -q '/00000000800003e0/' "$TMP/exec.log"; then
    echo "FAIL the program didn't run (no block at 0x800003e0)"; exit 1
fi
if grep -q '/0000000080000400/' "$TMP/exec.log"; then
    echo "FAIL a block starts at 0x80000400: the machine runs on 1K pages"; exit 1
fi
echo "PASS no block starts at the 1K boundary 0x80000400"
