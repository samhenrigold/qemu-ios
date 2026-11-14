@ Checks for tests/ipod/tcg-sync-test.sh: a bare-metal Cortex-A8 program run as the bootrom (at 0, SVC mode; RAM at
@ RAM, -DRAM=). TCG stores the guest registers and flags a memory access may need only on its slow path; these
@ check that what an exception, a rewound device read and a TLB miss see is what the guest computed: registers and
@ flags set just before a faulting load or store, whether used again or overwritten after it, the base of a load
@ into itself, registers live across slow paths, and a device read part way through a block. One character per
@ check on the semihosting console: its letter in lowercase if it passed, uppercase if not; then 'E', and it exits.
        .syntax unified
        .arch armv7-a
        .text
        .arm
        .equ T, RAM + 0x20000           @ L1 table: identity sections, nothing at 0x70000000
        .equ BAD, 0x70000000            @ unmapped
        .equ REC, RAM + 0x100           @ at each data abort: count, r0-r7, SPSR, return address
        .equ PAGES, RAM + 0x80000       @ 16 pages of RAM, read after each TLBIALL
        .equ CDMA1, 0x87001000          @ S5L8930 CDMA channel 1 (SETTINGS at +4)
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
@ a data abort: count it, record r0-r7, SPSR and the return address, resume after the access (ARM or Thumb)
.Ldabt:
        push    {r8, r9}
        ldr     r8, =REC
        ldr     r9, [r8]
        add     r9, r9, #1
        str     r9, [r8]
        add     r9, r8, #4
        stm     r9, {r0-r7}
        mrs     r9, spsr
        str     r9, [r8, #36]
        sub     r9, lr, #8
        str     r9, [r8, #40]
        mrs     r9, spsr
        tst     r9, #0x20
        pop     {r8, r9}
        subsne  pc, lr, #6              @ Thumb: a 2-byte access insn after the faulting one (the checks use 16-bit)
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

@ r11 += 1 unless \a == \b (r12 and the flags clobbered)
        .macro  expect a, b
        ldr     r12, =\b
        cmp     \a, r12
        addne   r11, r11, #1
        .endm

@ r11 += 1 unless exactly one abort was recorded since the last, with r\n = \v ...
        .macro  rec n, v
        ldr     r9, =REC + 4 + 4 * \n
        ldr     r9, [r9]
        expect  r9, \v
        .endm
        .macro  rec_flags v                 @ NZCV
        ldr     r9, =REC + 36
        ldr     r9, [r9]
        lsr     r9, r9, #28
        expect  r9, \v
        .endm
        .macro  rec_one
        ldr     r8, =REC
        ldr     r9, [r8]
        expect  r9, 1
        mov     r9, #0
        str     r9, [r8]
        .endm

@ a: registers and flags set just before a faulting load: used after it, or overwritten after it
.La:
        push    {r4-r9, lr}
        mov     r11, #0
        ldr     r6, =BAD
        mov     r0, #0x11
        mov     r1, #0x22
        mov     r2, #0x33
        ldr     r3, =0x7fffffff
        adds    r3, r3, #1                  @ N and V
        add     r0, r0, #0x100
        add     r1, r1, #0x100
        ldr     r4, [r6]                    @ aborts
        mov     r0, #0                      @ r0, r1 and the flags overwritten
        mov     r1, #0
        movs    r2, r2                      @ C and V kept, N and Z from r2
        rec     0, 0x111
        rec     1, 0x122
        rec     2, 0x33
        rec     3, 0x80000000
        rec_flags 0x9
        rec_one
        @ the same with C and Z, used after it
        mov     r0, #0x44
        subs    r1, r0, #0x44               @ Z and C
        add     r2, r0, #1
        str     r2, [r6, #4]                @ aborts (a store)
        add     r5, r0, r2
        rec     0, 0x44
        rec     1, 0
        rec     2, 0x45
        rec_flags 0x6
        rec_one
        expect  r5, 0x89
        mov     r2, #'a'
        bl      .Lverdict
        pop     {r4-r9, pc}

@ b: a load into its own base, and into a register set just before, abort with both as they were
.Lb:
        push    {r4-r9, lr}
        mov     r11, #0
        ldr     r0, =BAD + 8
        add     r0, r0, #4                  @ BAD + 12, not yet in memory
        mov     r3, #0x99
        add     r3, r3, #1
        ldr     r0, [r0]                    @ aborts
        ldr     r3, [r0, #-12]              @ aborts too: r0 is still BAD + 12
        rec     0, BAD + 12
        rec     3, 0x9a
        ldr     r8, =REC
        ldr     r9, [r8]
        expect  r9, 2
        mov     r9, #0
        str     r9, [r8]
        @ post-indexed writeback: not done when the load aborts
        ldr     r1, =BAD
        ldr     r2, [r1], #4
        rec     1, BAD
        rec_one
        expect  r1, BAD
        mov     r2, #'b'
        bl      .Lverdict
        pop     {r4-r9, pc}

@ c: the same in Thumb
.Lc:
        push    {r4-r9, lr}
        mov     r11, #0
        ldr     r0, =.Lc_thumb
        blx     r0
        rec     0, 0x5
        rec     1, 0x77
        rec_flags 0x2                       @ C
        rec_one
        expect  r0, 0x1234
        mov     r2, #'c'
        bl      .Lverdict
        pop     {r4-r9, pc}

@ d: twelve registers live across loads that miss the TLB (TLBIALL before each pass), summed after
.Ld:
        push    {r4-r11, lr}
        ldr     r0, =PAGES
        mov     r1, #0
        mov     r2, #16
1:      str     r2, [r0, r1]
        add     r1, r1, #0x1000
        subs    r2, r2, #1
        bne     1b
        mov     lr, #20
2:      mcr     p15, 0, r0, c8, c7, 0       @ TLBIALL
        isb
        ldr     r12, =PAGES
        mov     r0, #1
        mov     r1, #2
        mov     r2, #3
        mov     r3, #4
        mov     r4, #5
        mov     r5, #6
        mov     r6, #7
        mov     r7, #8
        mov     r8, #9
        mov     r9, #10
        mov     r10, #11
        mov     r11, #12
        ldr     r0, [r12]                   @ 16
        add     r1, r1, r0
        add     r12, r12, #0x1000
        ldr     r2, [r12]        @ 15 (a new page)
        add     r3, r3, r2
        add     r12, r12, #0x1000
        ldr     r4, [r12]        @ 14
        add     r5, r5, r4
        add     r12, r12, #0x1000
        ldr     r6, [r12]        @ 13
        add     r7, r7, r6
        add     r12, r12, #0x1000
        ldr     r8, [r12]        @ 12
        add     r9, r9, r8
        add     r12, r12, #0x1000
        ldr     r10, [r12]       @ 11
        add     r11, r11, r10
        add     r0, r0, r1
        add     r0, r0, r2
        add     r0, r0, r3
        add     r0, r0, r4
        add     r0, r0, r5
        add     r0, r0, r6
        add     r0, r0, r7
        add     r0, r0, r8
        add     r0, r0, r9
        add     r0, r0, r10
        add     r0, r0, r11
        ldr     r1, =16 + 18 + 15 + 19 + 14 + 20 + 13 + 21 + 12 + 22 + 11 + 23
        cmp     r0, r1
        bne     3f
        subs    lr, lr, #1
        bne     2b
3:      mov     r11, lr                     @ 0 if every pass summed right
        mov     r2, #'d'
        bl      .Lverdict
        pop     {r4-r11, pc}

@ f: a device read part way through a block (rewound to end the block there) between updates of one register and
@ the flags, 100 times
.Lf:
        push    {r4-r9, lr}
        mov     r11, #0
        ldr     r8, =CDMA1
        mov     r0, #0
        str     r0, [r8, #4]                @ SETTINGS
        mov     r5, #100
        mov     r1, #0
        mov     r3, #0
1:      add     r1, r1, #1
        adds    r4, r1, #0x80000000         @ C clear, N set
        ldr     r2, [r8, #4]                @ the device
        add     r1, r1, #2
        addmi   r3, r3, #1
        subs    r5, r5, #1
        bne     1b
        expect  r1, 300
        expect  r3, 100
        mov     r2, #'f'
        bl      .Lverdict
        pop     {r4-r9, pc}

@ g: a device store part way through a block, then a load from it, with flags set before and used after
.Lg:
        push    {r4-r9, lr}
        mov     r11, #0
        ldr     r8, =CDMA1
        mov     r5, #50
        mov     r1, #0
1:      add     r1, r1, #5
        cmp     r1, #100                    @ C set from r1 >= 100
        str     r1, [r8, #8]                @ FIFO
        ldr     r2, [r8, #8]
        addhs   r1, r1, #1
        add     r1, r1, r2
        sub     r1, r1, r2
        subs    r5, r5, #1
        bne     1b
        expect  r1, 250 + 31
        mov     r2, #'g'
        bl      .Lverdict
        pop     {r4-r9, pc}
        .ltorg

        .thumb
        .thumb_func
.Lc_thumb:
        push    {r4-r7, lr}
        ldr     r4, =BAD
        movs    r0, #2
        movs    r1, #0x77
        adds    r0, r0, #3                  @ 5, flags clear
        cmp     r0, #1                      @ C
        ldr     r2, [r4]                    @ aborts
        movs    r0, #0                      @ overwrites r0 and the flags
        ldr     r0, =0x1234
        pop     {r4-r7, pc}
        .ltorg
