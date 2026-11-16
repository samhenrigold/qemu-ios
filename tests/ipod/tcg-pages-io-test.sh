#!/bin/bash
# GATE: models
# The iPod's translations: 4K pages, and a polled device read that ends its block instead of rewinding it.
#
# The ARM1176 can't map a page under 4K (VMSAv6 has no tiny pages), so the iPod machine runs on 4K target pages: a
# block may run on past a 1K boundary, and a branch within 4K chains. And an MMIO load must be a block's last insn;
# one found mid-block is rewound (cpu_io_recompile) and run alone, which a device polled in a loop (the kernel's
# timebase read) paid on every pass until the block is retranslated to end at the load.
#
# A bare-metal program as the iPod's bootrom (staged at 0, MMU off) runs nine NOPs across 0x400, then reads CHIPID
# a thousand times mid-block, and spins. Under -d exec, no block may start at 0x400 (the page boundary of a 1K page)
# and at most a few loads may be rewound. QEMU= selects the binary (default build/qemu-system-arm).
set -u
QEMU="${QEMU:-$(cd "$(dirname "$0")/../.." && pwd)/build/qemu-system-arm}"
[ -x "$QEMU" ] || { echo "FAIL no emulator at $QEMU: build it or set QEMU="; exit 1; }
TMP="$(mktemp -d "${TMPDIR:-/tmp}/tcgio.XXXXXX")"
pid=
cleanup() {
    if [ -n "$pid" ]; then
        kill "$pid" 2>/dev/null || true
        wait "$pid" 2>/dev/null || true
    fi
    rm -rf "$TMP"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
words() { for word in "$@"; do printf '%s' "${word:6:2}${word:4:2}${word:2:2}${word:0:2}"; done; }
{
    words e3a0343d e3833601 e3a04ffa ea0000f3   # r3 = CHIPID (0x3d100000); r4 = 1000; b 0x3e0
    head -c $((0x3e0 - 16)) /dev/zero | xxd -p | tr -d '\n'
    words e1a00000 e1a00000 e1a00000 e1a00000 e1a00000 e1a00000 e1a00000 e1a00000 e1a00000   # 0x3e0-0x400: nop
    words e2811001 e5932004 e2811001 e2544001 1afffffa   # 0x404: add; ldr r2,[r3,#4]; add; subs r4,#1; bne 0x404
    words eafffffe                                        # b .
} | xxd -r -p > "$TMP/rom.bin"
truncate -s 131072 "$TMP/rom.bin"
truncate -s 1048576 "$TMP/nor.bin"
mkdir "$TMP/nand"
"$QEMU" -M "iPod-Touch,bootrom=$TMP/rom.bin,nor=$TMP/nor.bin,nand=$TMP/nand" -display none -monitor none \
    -serial none -nic none -audio driver=none -d exec -D "$TMP/exec.log" </dev/null >/dev/null 2>&1 &
pid=$!
for _ in $(seq 50); do grep -q '/0000000000000418/' "$TMP/exec.log" 2>/dev/null && break; sleep 0.2; done
kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null
pid=
grep -q '/0000000000000418/' "$TMP/exec.log" || { echo "FAIL the program never reached its end"; exit 1; }
rewound=$(grep -c 'cpu_io_recompile: rewound' "$TMP/exec.log")
fail=0
if grep -q '/0000000000000400/' "$TMP/exec.log"; then
    echo "FAIL a block starts at 0x400: the iPod runs on 1K pages"; fail=1
else
    echo "PASS no block starts at 0x400 (4K pages)"
fi
if [ "$rewound" -gt 4 ]; then
    echo "FAIL $rewound of 1000 polled reads rewound their block"; fail=1
else
    echo "PASS $rewound of 1000 polled reads rewound their block"
fi
exit $fail
