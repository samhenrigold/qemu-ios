@ Checks for tests/ipod/tcg-ldm-test.sh: a bare-metal Cortex-A8 program run as the bootrom (at 0, SVC mode; RAM at
@ RAM, -DRAM=) whose LDM, STM, PUSH and POP TCG moves two or four words at a time on the fast path. Each check
@ compares a block transfer with the same words moved by single LDR and STR, or with what the architecture (and
@ QEMU before the change) does at a fault: a data abort part way through, at a page boundary, a misaligned base,
@ device registers, code rewritten by STM, IT blocks and the user-mode forms. One character per check on the
@ semihosting console: its letter in lowercase if it passed, uppercase if not; then 'E', and it exits.
        .syntax unified
        .arch armv7-a
        .text
        .arm
        .equ T, RAM + 0x20000           @ L1 table: identity sections, an L2 table at 0x70000000
        .equ L2, RAM + 0x24000
        .equ VA_P, 0x70001000           @ mapped (to PA_P); the page after it is not
        .equ VA_END, VA_P + 0x1000
        .equ PA_P, RAM + 0x30000
        .equ M, RAM + 0x50100           @ the middle of 128 words a block transfer works on
        .equ R, RAM + 0x51100           @ the same, moved by single LDR and STR
        .equ CHK, RAM + 0x52000         @ registers after a block load, then after the single loads
        .equ CHK2, RAM + 0x52100
        .equ CODE, RAM + 0x60000        @ a function rewritten by STM
        .equ REC, RAM + 0x100           @ data aborts: count, DFAR, DFSR, return address
        .equ CDMA1, 0x87001000          @ S5L8930 CDMA channel 1: SETTINGS, FIFO, REMAIN, ADDR, DESC at +4..+0x14
        .equ PG, 0x072                  @ small page: AP=11, TEX=001, global
        .equ SECT, 0x1c02               @ section: AP=11, TEX=001, domain 0, global
        .equ DEV, 0x0c16                @ section: AP=11, device (B), XN

_start:
        b       .Lreset
        b       .
        b       .
        b       .
        b       .Ldabt
        b       .
        b       .
        b       .
@ a data abort (from ARM code): count it, record DFAR, DFSR and the return address, resume after the access
.Ldabt:
        push    {r0, r1}
        ldr     r1, =REC
        ldr     r0, [r1]
        add     r0, r0, #1
        str     r0, [r1]
        mrc     p15, 0, r0, c6, c0, 0
        str     r0, [r1, #4]
        mrc     p15, 0, r0, c5, c0, 0
        str     r0, [r1, #8]
        sub     r0, lr, #8
        str     r0, [r1, #12]
        pop     {r0, r1}
        subs    pc, lr, #4

.Lreset:
        cpsid   aif
        cps     #0x17
        ldr     sp, =RAM + 0xe000
        cps     #0x13
        ldr     sp, =RAM + 0xf000
        mov     r10, #0
        ldr     r0, =T
        mov     r1, #0
        mov     r2, #0x4000
1:      str     r1, [r0], #4
        subs    r2, r2, #4
        bne     1b
        ldr     r0, =T
        ldr     r1, =SECT
        str     r1, [r0]                    @ the ROM
        mov     r2, #0x400
2:      orr     r3, r1, r2, lsl #20         @ the RAM
        str     r3, [r0, r2, lsl #2]
        add     r2, r2, #1
        cmp     r2, #0x500
        bne     2b
        ldr     r1, =0x87000000 | DEV       @ the CDMA
        ldr     r2, =T + 0x870 * 4
        str     r1, [r2]
        ldr     r1, =L2 | 1
        add     r2, r0, #0x700 * 4
        str     r1, [r2]
        ldr     r0, =L2
        mov     r1, #0
        mov     r2, #0x400
3:      str     r1, [r0], #4
        subs    r2, r2, #4
        bne     3b
        ldr     r0, =L2
        ldr     r1, =PA_P | PG
        str     r1, [r0, #4]
        ldr     r0, =T
        mcr     p15, 0, r0, c2, c0, 0       @ TTBR0
        mov     r0, #0
        mcr     p15, 0, r0, c2, c0, 2       @ TTBCR
        mov     r0, #1
        mcr     p15, 0, r0, c3, c0, 0       @ DACR: domain 0 client
        mcr     p15, 0, r0, c8, c7, 0       @ TLBIALL
        mrc     p15, 0, r0, c1, c0, 0
        bic     r0, r0, #0x2000
        orr     r0, r0, #1
        mcr     p15, 0, r0, c1, c0, 0
        isb
        bl      .La
        bl      .Lb
        bl      .Lc
        bl      .Ld
        bl      .Lf
        bl      .Lg
        bl      .Lh
        bl      .Li
        bl      .Lk
        ldr     r0, =.Ll
        blx     r0
        bl      .Lm
        mov     r0, #'E'
        bl      .Lputc
        mov     r0, #0x18                   @ SYS_EXIT
        ldr     r1, =0x20026                @ ADP_Stopped_ApplicationExit
        svc     0x123456
        b       .

@ r11 = mismatches, r2 = the check's letter
.Lverdict:
        mov     r0, r2
        cmp     r11, #0
        subne   r0, r0, #0x20
        addne   r10, r10, #1
.Lputc:
        push    {r0, r1, lr}
        mov     r1, sp
        mov     r0, #0x03                   @ SYS_WRITEC
        svc     0x123456
        pop     {r0, r1, pc}

@ r11 += 1 unless r0 == r1 (flags clobbered)
        .macro  expect a, b
        cmp     \a, \b
        addne   r11, r11, #1
        .endm

@ fill M and R (128 words each, from 64 words below the middle) with r0, r0 + 1, ...
.Lfill:
        push    {r0-r3}
        ldr     r1, =M - 0x100
        ldr     r2, =R - 0x100
        mov     r3, #128
1:      str     r0, [r1], #4
        str     r0, [r2], #4
        add     r0, r0, #1
        subs    r3, r3, #1
        bne     1b
        pop     {r0-r3}
        bx      lr

@ r11 += the words that differ between M and R; and between CHK and CHK2 (16 words each)
.Lcompare:
        push    {r0-r4}
        ldr     r1, =M - 0x100
        ldr     r2, =R - 0x100
        mov     r3, #128
1:      ldr     r0, [r1], #4
        ldr     r4, [r2], #4
        expect  r0, r4
        subs    r3, r3, #1
        bne     1b
        ldr     r1, =CHK
        ldr     r2, =CHK2
        mov     r3, #16
2:      ldr     r0, [r1], #4
        ldr     r4, [r2], #4
        expect  r0, r4
        subs    r3, r3, #1
        bne     2b
        pop     {r0-r4}
        bx      lr

@ r0-r7 = distinct values, r12 = 0x3a000000 + \tag
        .macro  setregs tag
        ldr     r0, =0x10000000 + \tag
        add     r1, r0, #0x01000000
        add     r2, r0, #0x02000000
        add     r3, r0, #0x03000000
        add     r4, r0, #0x04000000
        add     r5, r0, #0x05000000
        add     r6, r0, #0x06000000
        add     r7, r0, #0x07000000
        .endm

@ store r0-r7 to \where
        .macro  saveregs where
        push    {r8}
        ldr     r8, =\where
        str     r0, [r8]
        str     r1, [r8, #4]
        str     r2, [r8, #8]
        str     r3, [r8, #12]
        str     r4, [r8, #16]
        str     r5, [r8, #20]
        str     r6, [r8, #24]
        str     r7, [r8, #28]
        pop     {r8}
        .endm

@ a: A32 STM, four addressing modes with writeback, lists of 8, 3 and 6 registers, against single stores
.La:
        push    {lr}
        mov     r11, #0
        mov     r0, #0
        bl      .Lfill
        setregs 0x100
        ldr     r8, =M
        stmia   r8!, {r0-r7}
        ldr     r9, =R
        str     r0, [r9]
        str     r1, [r9, #4]
        str     r2, [r9, #8]
        str     r3, [r9, #12]
        str     r4, [r9, #16]
        str     r5, [r9, #20]
        str     r6, [r9, #24]
        str     r7, [r9, #28]
        ldr     r9, =M + 32
        expect  r8, r9
        setregs 0x200
        ldr     r8, =M
        stmib   r8!, {r1, r3, r5}
        ldr     r9, =R
        str     r1, [r9, #4]
        str     r3, [r9, #8]
        str     r5, [r9, #12]
        ldr     r9, =M + 12
        expect  r8, r9
        setregs 0x300
        ldr     r8, =M
        stmda   r8!, {r0-r4, r6}
        ldr     r9, =R
        str     r0, [r9, #-20]
        str     r1, [r9, #-16]
        str     r2, [r9, #-12]
        str     r3, [r9, #-8]
        str     r4, [r9, #-4]
        str     r6, [r9]
        ldr     r9, =M - 24
        expect  r8, r9
        setregs 0x400
        ldr     r8, =M - 0x80
        stmdb   r8!, {r0-r7}
        ldr     r9, =R - 0x80
        str     r0, [r9, #-32]
        str     r1, [r9, #-28]
        str     r2, [r9, #-24]
        str     r3, [r9, #-20]
        str     r4, [r9, #-16]
        str     r5, [r9, #-12]
        str     r6, [r9, #-8]
        str     r7, [r9, #-4]
        ldr     r9, =M - 0x80 - 32
        expect  r8, r9
        setregs 0x500                       @ 2 words, no writeback, at an address 4 mod 8
        ldr     r8, =M + 0x44
        stmia   r8, {r6, r7}
        ldr     r9, =R + 0x44
        str     r6, [r9]
        str     r7, [r9, #4]
        ldr     r9, =M + 0x44
        expect  r8, r9
        bl      .Lcompare
        mov     r2, #'a'
        bl      .Lverdict
        pop     {pc}

@ b: A32 LDM, the same modes and lists, against single loads
.Lb:
        push    {lr}
        mov     r11, #0
        ldr     r0, =0xb0000000
        bl      .Lfill
        setregs 0x99
        ldr     r8, =M
        ldmia   r8!, {r0-r7}
        saveregs CHK
        ldr     r9, =M + 32
        expect  r8, r9
        ldr     r9, =M
        ldr     r0, [r9]
        ldr     r1, [r9, #4]
        ldr     r2, [r9, #8]
        ldr     r3, [r9, #12]
        ldr     r4, [r9, #16]
        ldr     r5, [r9, #20]
        ldr     r6, [r9, #24]
        ldr     r7, [r9, #28]
        saveregs CHK2
        bl      .Lcompare
        setregs 0x98
        ldr     r8, =M
        ldmib   r8!, {r1, r3, r5}
        saveregs CHK
        ldr     r9, =M + 12
        expect  r8, r9
        setregs 0x98
        ldr     r9, =M
        ldr     r1, [r9, #4]
        ldr     r3, [r9, #8]
        ldr     r5, [r9, #12]
        saveregs CHK2
        bl      .Lcompare
        setregs 0x97
        ldr     r8, =M
        ldmda   r8!, {r0-r4, r6}
        saveregs CHK
        ldr     r9, =M - 24
        expect  r8, r9
        setregs 0x97
        ldr     r9, =M
        ldr     r0, [r9, #-20]
        ldr     r1, [r9, #-16]
        ldr     r2, [r9, #-12]
        ldr     r3, [r9, #-8]
        ldr     r4, [r9, #-4]
        ldr     r6, [r9]
        saveregs CHK2
        bl      .Lcompare
        setregs 0x96
        ldr     r8, =M
        ldmdb   r8!, {r0-r7}
        saveregs CHK
        ldr     r9, =M - 32
        expect  r8, r9
        ldr     r9, =M
        ldr     r0, [r9, #-32]
        ldr     r1, [r9, #-28]
        ldr     r2, [r9, #-24]
        ldr     r3, [r9, #-20]
        ldr     r4, [r9, #-16]
        ldr     r5, [r9, #-12]
        ldr     r6, [r9, #-8]
        ldr     r7, [r9, #-4]
        saveregs CHK2
        bl      .Lcompare
        mov     r2, #'b'
        bl      .Lverdict
        pop     {pc}

@ c: Thumb PUSH/POP (with LR and PC) and T32 LDM/STM, through a call that returns with pop {..pc}
.Lc:
        push    {lr}
        mov     r11, #0
        ldr     r0, =0xc0000000
        bl      .Lfill
        ldr     r0, =.Lc_thumb
        blx     r0
        bl      .Lcompare
        mov     r2, #'c'
        bl      .Lverdict
        pop     {pc}

@ d: the base register in the list: A32 LDM without writeback loads it, T16 LDM loads it and doesn't write back;
@ e: A32 STM with the base lowest in the list and writeback stores the base's original value
.Ld:
        push    {lr}
        mov     r11, #0
        ldr     r0, =0xd0000000
        bl      .Lfill
        ldr     r2, =M
        ldmia   r2, {r0-r3}                 @ r2 = M[2]
        ldr     r9, =0xd0000040 + 2
        expect  r2, r9
        ldr     r9, =0xd0000043
        expect  r3, r9
        ldr     r0, =.Ld_thumb
        blx     r0
        ldr     r0, =M + 0x10
        mov     r1, #0x55
        mov     r2, #0x66
        mov     r3, #0x77
        stmia   r0!, {r0-r3}
        ldr     r9, =M + 0x20
        expect  r0, r9
        ldr     r9, =M + 0x10
        ldr     r1, [r9]
        expect  r1, r9                      @ the original base
        ldr     r1, [r9, #12]
        expect  r1, #0x77
        mov     r2, #'d'
        bl      .Lverdict
        pop     {pc}

@ f: LDM aborts at the first word of an unmapped page: DFAR that word, a read, the base not written back, the PC
@ not loaded; for 8 words (two on the mapped page), 4 (three on it), and 4 with the PC last
.Lf:
        push    {lr}
        mov     r11, #0
        ldr     r9, =REC
        mov     r0, #0
        str     r0, [r9]
        ldr     r8, =VA_END - 8
.Lf1:   ldmia   r8!, {r0-r7}
        ldr     r9, =VA_END - 8
        expect  r8, r9
        ldr     r9, =.Lf1
        bl      .Lone_abort
        ldr     r8, =VA_END - 12
.Lf2:   ldmia   r8!, {r0-r3}
        ldr     r9, =VA_END - 12
        expect  r8, r9
        ldr     r9, =.Lf2
        bl      .Lone_abort
        ldr     r8, =VA_END - 12
.Lf3:   ldmia   r8, {r0-r2, pc}             @ resumes at the next instruction, the PC unchanged
        ldr     r9, =.Lf3
        bl      .Lone_abort
        ldr     r8, =VA_END
.Lf4:   ldmdb   r8!, {r0-r3}                @ all mapped: no abort
        ldr     r9, =VA_END - 16
        expect  r8, r9
        ldr     r9, =REC
        ldr     r0, [r9]
        expect  r0, #0
        mov     r2, #'f'
        bl      .Lverdict
        pop     {pc}

@ r11 += 1 unless exactly one read abort was recorded, at VA_END from the instruction at r9; clears the record
.Lone_abort:
        ldr     r1, =REC
        ldr     r0, [r1]
        expect  r0, #1
        ldr     r0, [r1, #4]
        ldr     r2, =VA_END
        expect  r0, r2
        ldr     r0, [r1, #8]
        ldr     r2, =0x80f
        and     r0, r0, r2
        expect  r0, #0x007                  @ translation fault (page), a read
        ldr     r0, [r1, #12]
        expect  r0, r9
        mov     r0, #0
        str     r0, [r1]
        bx      lr

@ g: STM aborts at the first word of an unmapped page having stored the words before it, as single stores do;
@ the base isn't written back and DFSR says a write
.Lg:
        push    {lr}
        mov     r11, #0
        ldr     r9, =REC
        mov     r0, #0
        str     r0, [r9]
        ldr     r9, =VA_END - 16
        str     r0, [r9]
        str     r0, [r9, #4]
        str     r0, [r9, #8]
        str     r0, [r9, #12]
        setregs 0x6600
        ldr     r8, =VA_END - 12
.Lg1:   stmia   r8!, {r0-r7}
        ldr     r9, =VA_END - 12
        expect  r8, r9
        ldr     r9, =VA_END - 16
        ldr     r12, [r9]
        expect  r12, #0
        ldr     r12, [r9, #4]
        expect  r12, r0
        ldr     r12, [r9, #8]
        expect  r12, r1
        ldr     r12, [r9, #12]
        expect  r12, r2
        ldr     r1, =REC
        ldr     r0, [r1]
        expect  r0, #1
        ldr     r0, [r1, #4]
        ldr     r2, =VA_END
        expect  r0, r2
        ldr     r0, [r1, #8]
        ldr     r2, =0x80f
        and     r0, r0, r2
        ldr     r2, =0x807
        expect  r0, r2                      @ translation fault (page), a write
        ldr     r0, [r1, #12]
        ldr     r2, =.Lg1
        expect  r0, r2
        mov     r2, #'g'
        bl      .Lverdict
        pop     {pc}

@ h: a misaligned base: an alignment fault at the base for LDM and STM of 2 and 4 words, nothing stored
.Lh:
        push    {lr}
        mov     r11, #0
        ldr     r0, =0x88000000
        bl      .Lfill
        ldr     r9, =REC
        mov     r0, #0
        str     r0, [r9]
        ldr     r8, =M + 2
        ldmia   r8, {r0-r3}
        mov     r9, r8
        bl      .Lalign_abort
        ldr     r8, =M + 6
        ldmia   r8, {r0, r1}
        mov     r9, r8
        bl      .Lalign_abort
        ldr     r8, =M + 1
        stmia   r8!, {r0-r3}
        ldr     r9, =M + 1
        expect  r8, r9
        bl      .Lalign_abort
        ldr     r8, =M + 0x23
        stmdb   r8, {r4, r5}
        sub     r9, r8, #8
        bl      .Lalign_abort
        bl      .Lcompare                   @ nothing was stored
        mov     r2, #'h'
        bl      .Lverdict
        pop     {pc}

@ r11 += 1 unless exactly one alignment fault was recorded, at r9; clears the record
.Lalign_abort:
        ldr     r1, =REC
        ldr     r0, [r1]
        expect  r0, #1
        ldr     r0, [r1, #8]
        ldr     r2, =0x40f
        and     r0, r0, r2
        expect  r0, #0x001
        ldr     r0, [r1, #4]
        expect  r0, r9
        mov     r0, #0
        str     r0, [r1]
        bx      lr

@ i: device registers (CDMA channel 1, 4-byte accesses only): STM of 4 and 2 words and LDM back, no abort
.Li:
        push    {lr}
        mov     r11, #0
        ldr     r9, =REC
        mov     r0, #0
        str     r0, [r9]
        ldr     r8, =CDMA1 + 4
        mov     r0, #0                      @ SETTINGS
        ldr     r1, =0x11223344             @ FIFO
        ldr     r2, =0x00001000             @ REMAIN
        ldr     r3, =0x40001230             @ ADDR
        stmia   r8, {r0-r3}
        ldmia   r8, {r4-r7}
        expect  r4, r0
        expect  r5, r1
        expect  r6, r2
        expect  r7, r3
        ldr     r8, =CDMA1 + 0x10
        ldr     r0, =0x40002000
        ldr     r1, =0x40003000             @ DESC
        stmia   r8, {r0, r1}
        mov     r4, #0
        mov     r5, #0
        ldmia   r8, {r4, r5}
        expect  r4, r0
        expect  r5, r1
        ldr     r8, =CDMA1 + 8
        ldmia   r8, {r4, r5}                @ FIFO, REMAIN
        ldr     r0, =0x11223344
        expect  r4, r0
        expect  r5, #0x1000
        ldr     r9, =REC
        ldr     r0, [r9]
        expect  r0, #0
        mov     r2, #'i'
        bl      .Lverdict
        pop     {pc}

@ k: a function in RAM, run 100 times, rewritten by an STM of 2 words and then of 4, runs its new code
.Lk:
        push    {r4-r8, lr}
        mov     r11, #0
        ldr     r8, =CODE
        ldr     r4, =0xe3a00001             @ mov r0, #1
        ldr     r5, =0xe12fff1e             @ bx lr
        stmia   r8, {r4, r5}
        mcr     p15, 0, r0, c7, c5, 0       @ ICIALLU
        isb
        mov     r6, #0
        mov     r7, #100
1:      blx     r8
        add     r6, r6, r0
        subs    r7, r7, #1
        bne     1b
        ldr     r4, =0xe3a00002             @ mov r0, #2
        stmia   r8, {r4, r5}
        mcr     p15, 0, r0, c7, c5, 0
        isb
        blx     r8
        add     r6, r6, r0
        ldr     r2, =0xe3a00003             @ mov r0, #3
        ldr     r3, =0xe2800004             @ add r0, r0, #4
        ldr     r4, =0xe12fff1e             @ bx lr
        mov     r5, #0
        stmia   r8, {r2-r5}
        mcr     p15, 0, r0, c7, c5, 0
        isb
        blx     r8
        add     r6, r6, r0
        ldr     r0, =100 + 2 + 7
        expect  r6, r0
        mov     r2, #'k'
        bl      .Lverdict
        pop     {r4-r8, pc}

@ m: STM^ and LDM^ move the user-mode SP and LR (from SVC mode, whose own are untouched), 2 and 3 words
.Lm:
        push    {r4-r8, lr}
        mov     r11, #0
        cps     #0x1f                       @ System: the user registers
        ldr     sp, =0x5a5a0013
        ldr     lr, =0x5a5a0014
        cps     #0x13
        mov     r4, sp
        ldr     r8, =M
        stmia   r8, {r13, r14}^
        ldr     r0, [r8]
        ldr     r1, =0x5a5a0013
        expect  r0, r1
        ldr     r0, [r8, #4]
        ldr     r1, =0x5a5a0014
        expect  r0, r1
        ldr     r0, =0x6b6b0012
        ldr     r1, =0x6b6b0013
        ldr     r2, =0x6b6b0014
        stmia   r8, {r0-r2}
        ldmia   r8, {r12-r14}^
        expect  sp, r4
        ldr     r1, =0x6b6b0012
        expect  r12, r1
        cps     #0x1f
        mov     r0, sp
        mov     r1, lr
        cps     #0x13
        ldr     r2, =0x6b6b0013
        expect  r0, r2
        ldr     r2, =0x6b6b0014
        expect  r1, r2
        mov     r2, #'m'
        bl      .Lverdict
        pop     {r4-r8, pc}
        .ltorg

        .thumb
        .thumb_func
.Lc_thumb:
        push    {r4-r11, lr}
        setregs 0xc1
        ldr     r8, =M
        stmia.w r8!, {r0-r7}
        ldr     r9, =R
        str     r0, [r9]
        str     r1, [r9, #4]
        str     r2, [r9, #8]
        str     r3, [r9, #12]
        str     r4, [r9, #16]
        str     r5, [r9, #20]
        str     r6, [r9, #24]
        str     r7, [r9, #28]
        ldr     r9, =M + 32
        cmp     r8, r9
        it      ne
        addne   r11, r11, #1
        @ a push of 5 (4 + 1) and a pop of them with the PC, through a call, and back
        mov     r6, sp
        bl      .Lc_callee
        cmp     r0, #0x7c
        it      ne
        addne   r11, r11, #1
        cmp     r6, sp
        it      ne
        addne   r11, r11, #1
        @ T16 ldm with writeback, 3 words; T32 ldmdb, 6
        setregs 0xc2
        ldr     r4, =M + 0x40
        ldmia   r4!, {r0, r1, r3}
        ldr     r5, =M + 0x4c
        cmp     r4, r5
        it      ne
        addne   r11, r11, #1
        ldr     r5, =M + 0x40
        ldr     r2, [r5]
        cmp     r0, r2
        it      ne
        addne   r11, r11, #1
        ldr     r2, [r5, #8]
        cmp     r3, r2
        it      ne
        addne   r11, r11, #1
        ldr     r8, =M + 0x80
        ldmdb.w r8, {r0-r5}
        ldr     r9, =M + 0x80 - 24
        ldr     r7, [r9]
        cmp     r0, r7
        it      ne
        addne   r11, r11, #1
        ldr     r7, [r9, #20]
        cmp     r5, r7
        it      ne
        addne   r11, r11, #1
        @ ldm and stm in IT blocks, executed and skipped
        ldr     r8, =M + 0xc0
        movs    r0, #1
        movs    r1, #2
        movs    r2, #0x33
        movs    r3, #0x44
        cmp     r0, #1
        it      eq
        stmiaeq r8!, {r2, r3}
        it      ne
        stmiane r8!, {r0, r1}
        ldr     r9, =R + 0xc0
        str     r2, [r9]
        str     r3, [r9, #4]
        ldr     r9, =M + 0xc8
        cmp     r8, r9
        it      ne
        addne   r11, r11, #1
        movs    r2, #0
        movs    r3, #0
        ldr     r8, =M + 0xc0
        cmp     r0, #1
        itt     ne
        ldmiane r8, {r0, r1}
        movne   r0, #0x99
        it      eq
        ldmiaeq r8, {r2, r3}
        cmp     r2, #0x33
        it      ne
        addne   r11, r11, #1
        cmp     r3, #0x44
        it      ne
        addne   r11, r11, #1
        cmp     r0, #1
        it      ne
        addne   r11, r11, #1
        pop     {r4-r11, pc}

        .thumb_func
.Lc_callee:
        push    {r4-r7, lr}
        movs    r4, #0x7c
        movs    r0, #0
        cmp     r0, #1
        it      eq
        popeq   {r4-r7, pc}                 @ not taken
        mov     r0, r4
        pop     {r4-r7, pc}

        .thumb_func
.Ld_thumb:
        ldr     r2, =M + 0x20
        ldmia   r2, {r1, r2, r3}            @ base in the list: loaded, no writeback
        ldr     r0, =0xd0000048 + 1
        cmp     r2, r0
        it      ne
        addne   r11, r11, #1
        ldr     r0, =0xd0000048
        cmp     r1, r0
        it      ne
        addne   r11, r11, #1
        bx      lr

@ l: a conditional return by pop {pc} as the last instruction of an IT block, both ways
        .thumb_func
.Ll:
        push    {r4, lr}
        mov     r11, #0
        movs    r0, #1
        bl      .Ll_callee
        cmp     r0, #1
        it      ne
        addne   r11, r11, #1
        movs    r0, #0
        bl      .Ll_callee
        cmp     r0, #7
        it      ne
        addne   r11, r11, #1
        movs    r2, #'l'
        blx     .Lverdict
        pop     {r4, pc}
        .thumb_func
.Ll_callee:
        push    {r4, r5, lr}
        cmp     r0, #0
        it      ne
        popne   {r4, r5, pc}
        movs    r0, #7
        pop     {r4, r5, pc}
        .ltorg
