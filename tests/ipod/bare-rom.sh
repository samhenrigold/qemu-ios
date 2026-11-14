# Shared by the bare-metal TCG checks (tcg-asid-test.sh, tcg-psr-test.sh, tcg-pmu-test.sh), sourced after setting
# ROOT, SRC (the program) and CHECKS (one line per check: its letters, then what it checks). `boards` assembles SRC
# for the iPod touch 2G (ARM1176, RAM at 0x08000000) and the iPad (Cortex-A8, RAM at 0x40000000), runs it as each
# one's bootrom, prints PASS/FAIL per check and exits 1 on any FAIL (`board` runs one). The program prints each
# check's letter on the semihosting console, lowercase if it passed, then 'E'. Needs clang with the ARM target
# (Xcode's has it) and python3.
QEMU="${QEMU:-$ROOT/build/qemu-system-arm}"
[ -x "$QEMU" ] || { echo "FAIL no emulator at $QEMU: build it or set QEMU="; exit 1; }
TMP="$(mktemp -d "${TMPDIR:-/tmp}/barerom.XXXXXX")"
trap 'rm -rf "$TMP"' EXIT

# image RAM SIZE OUT: the program for a board with RAM at RAM, as a SIZE-byte bootrom
image() {
    clang --target=armv6k-none-eabi -c -DRAM="$1" -x assembler-with-cpp "$SRC" -o "$TMP/rom.o" ||
        return 1
    # .text, with the branches to its own labels resolved (clang leaves R_ARM_CALL/JUMP24 for a linker)
    python3 -I - "$TMP/rom.o" "$3" <<'PY' || return 1
import struct, sys
d = open(sys.argv[1], 'rb').read()
shoff, = struct.unpack_from('<I', d, 0x20)
shentsize, shnum, shstrndx = struct.unpack_from('<HHH', d, 0x2e)
secs = [struct.unpack_from('<IIIIIIIIII', d, shoff + i * shentsize) for i in range(shnum)]
name = lambda s: d[secs[shstrndx][4] + s[0]:].split(b'\0', 1)[0]
ti = next(i for i, s in enumerate(secs) if name(s) == b'.text')
text = bytearray(d[secs[ti][4]:secs[ti][4] + secs[ti][5]])
symtab = next(s for s in secs if s[1] == 2)
for s in secs:
    if s[1] == 9 and s[7] == ti:
        for k in range(s[5] // 8):
            off, info = struct.unpack_from('<II', d, s[4] + k * 8)
            _, value, _, _, _, shndx = struct.unpack_from('<IIIBBH', d, symtab[4] + (info >> 8) * 16)
            kind = info & 0xff
            assert kind in (0x02, 0x0a, 0x1c, 0x1d) and shndx == ti, 'unexpected relocation'
            insn, = struct.unpack_from('<I', text, off)
            if kind == 0x02:  # R_ARM_ABS32: .text is at 0
                struct.pack_into('<I', text, off, (insn + value) & 0xffffffff)
            elif kind == 0x0a:  # R_ARM_THM_CALL: bl to Thumb, blx to ARM
                hi, lo = insn & 0xffff, insn >> 16
                sg, j1, j2 = hi >> 10 & 1, lo >> 13 & 1, lo >> 11 & 1
                imm = sg << 24 | (1 - (j1 ^ sg)) << 23 | (1 - (j2 ^ sg)) << 22 | (hi & 0x3ff) << 12 | (lo & 0x7ff) << 1
                addend = (imm ^ 1 << 24) - (1 << 24)
                if value & 1:
                    disp, lo = (value & ~1) + addend - off, lo | 0x1000
                else:
                    disp, lo = value + addend - (off & ~3), lo & ~0x1000
                sg, i1, i2 = disp >> 24 & 1, disp >> 23 & 1, disp >> 22 & 1
                hi = 0xf000 | sg << 10 | (disp >> 12 & 0x3ff)
                lo = (lo & 0xd000) | (1 - (i1 ^ sg)) << 13 | (1 - (i2 ^ sg)) << 11 | (disp >> 1 & 0x7ff)
                struct.pack_into('<HH', text, off, hi, lo)
            else:
                addend = ((insn & 0xffffff) ^ 0x800000) - 0x800000
                struct.pack_into('<I', text, off, (insn & 0xff000000) | (((value - off >> 2) + addend) & 0xffffff))
open(sys.argv[2], 'wb').write(text)
PY
    truncate -s "$2" "$3"
}

fail=0
# board NAME MACHINE RAM ROMSIZE
board() {
    local out="$TMP/$1.out"
    if ! image "$3" "$4" "$TMP/$1.rom"; then
        echo "FAIL $1: couldn't build the program"; fail=1; return
    fi
    ${TIMEOUT:+$TIMEOUT 60} "$QEMU" -M "$2,bootrom=$TMP/$1.rom$5" -display none -monitor none -serial none -nic none \
        -audio driver=none -semihosting-config enable=on,target=native,chardev=sh -chardev "file,id=sh,path=$out" \
        </dev/null >/dev/null 2>&1
    local got; got="$(head -c 64 "$out" 2>/dev/null | tr -d '\n')"
    case "$got" in *E) ;; *) echo "FAIL $1: the program didn't finish (output '$got')"; fail=1; return ;; esac
    while read -r letters what; do
        local bad=0 i
        for ((i = 0; i < ${#letters}; i++)); do
            case "$got" in *"${letters:i:1}"*) ;; *) bad=1 ;; esac
        done
        if [ $bad = 0 ]; then echo "PASS $1: $what"; else echo "FAIL $1: $what ($got)"; fail=1; fi
    done <<< "$CHECKS"
}
boards() {
    mkdir "$TMP/nand"
    truncate -s 1048576 "$TMP/nor.bin"
    board ARM1176 iPod-Touch 0x08000000 131072 ",nor=$TMP/nor.bin,nand=$TMP/nand"
    board Cortex-A8 ipad1 0x40000000 65536 ""
    exit $fail
}
