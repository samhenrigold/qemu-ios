@ Checks for tests/ipod/tcg-probe-test.sh: a bare-metal Cortex-A8 program run as the bootrom (at 0, SVC mode, MMU off;
@ RAM at RAM, -DRAM=) whose branches leave their TB through TCG's jump cache probe: calls to other pages,
@ returns, interworking both ways, a conditional return in an IT block, an indirect call and a return into a
@ page remapped since the probe last found it, code at two virtual addresses that reads the PC, and three returns
@ that share one jump cache set. One character per check on the semihosting console: its letter in
@ lowercase if it passed, uppercase if not; then 'E', and it exits.
        .syntax unified
        .arch armv7-a
        .text
        .arm
        .equ T, RAM + 0x20000           @ L1 table: identity sections, an L2 table at 0x70000000
        .equ L2, RAM + 0x24000
        .equ PA_F1, RAM + 0x30000       @ VA_F's code at first: return 1
        .equ PA_F2, RAM + 0x31000       @ then: return 2
        .equ PA_R1, RAM + 0x32000       @ VA_R's code at first: call r4, then return 1
        .equ PA_R2, RAM + 0x33000       @ then: the same call, then return 2
        .equ PA_P, RAM + 0x34000        @ code that reads the PC, at VA_P1 and VA_P2
        .equ VA_F, 0x70001000
        .equ VA_R, 0x70002000
        .equ VA_P1, 0x70003000
        .equ VA_P2, 0x70004000
        .equ PG, 0x072                  @ small page: AP=11, TEX=001, global
        .equ SECT, 0x1c02               @ section: AP=11, TEX=001, domain 0, global

_start:
        cpsid   aif
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
        ldr     r1, =PA_F1 | PG
        str     r1, [r0, #4]
        ldr     r1, =PA_R1 | PG
        str     r1, [r0, #8]
        ldr     r1, =PA_P | PG
        str     r1, [r0, #12]
        str     r1, [r0, #16]
        @ the pages' code, from the templates below
        ldr     r0, =f1_code
        ldr     r1, =PA_F1
        bl      .Lcopy
        ldr     r0, =f2_code
        ldr     r1, =PA_F2
        bl      .Lcopy
        ldr     r0, =r1_code
        ldr     r1, =PA_R1
        bl      .Lcopy
        ldr     r0, =r2_code
        ldr     r1, =PA_R2
        bl      .Lcopy
        ldr     r0, =p_code
        ldr     r1, =PA_P
        bl      .Lcopy
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
        ldr     r0, =thumb_main             @ a .thumb_func symbol: bit 0 set
        blx     r0
        mov     r0, #'E'
        bl      .Lputc
        mov     r0, #0x18                   @ SYS_EXIT
        ldr     r1, =0x20026                @ ADP_Stopped_ApplicationExit
        svc     0x123456
        b       .

@ r0 = got, r1 = expected, r2 = the check's letter
.Lcheck:
        cmp     r0, r1
        subne   r2, r2, #0x20
        addne   r10, r10, #1
        mov     r0, r2
.Lputc:
        push    {r0, r1, lr}
        mov     r1, sp
        mov     r0, #0x03                   @ SYS_WRITEC
        svc     0x123456
        pop     {r0, r1, pc}

@ copy 64 bytes from r0 to r1
.Lcopy:
        mov     r2, #64
1:      ldr     r3, [r0], #4
        str     r3, [r1], #4
        subs    r2, r2, #4
        bne     1b
        bx      lr
        .ltorg

@ an ARM function in its own page: count, return to Thumb
        .balign 4096
arm_count:
        add     r9, r9, #1
        bx      lr

@ an ARM function that calls Thumb code through a register and returns with pop {pc}
arm_calls_thumb:
        push    {r4, lr}
        ldr     r4, =thumb_count
        blx     r4
        pop     {r4, pc}
        .ltorg

        .thumb
        .balign 4096
        .thumb_func
thumb_count:
        push    {r4, lr}
        adds    r9, #1
        pop     {r4, pc}

        .thumb_func
thumb_itret:                            @ count only when r0 != 0: the return when r0 == 0 is conditional, in an IT
        cmp     r0, #0
        it      eq
        bxeq    lr
        adds    r9, #1
        bx      lr

        .thumb_func
remap_r:                                @ map VA_R to PA_R2 and invalidate it, from inside VA_R's call
        ldr     r0, =L2
        ldr     r1, =PA_R2 | PG
        str     r1, [r0, #8]
        dsb
        ldr     r0, =VA_R
        mcr     p15, 0, r0, c8, c7, 1       @ TLBIMVA
        dsb
        isb
        bx      lr

        .thumb_func
nothing:
        bx      lr
        .ltorg

        .balign 4096
        .thumb_func
thumb_main:
        push    {r4-r8, lr}
        @ a: a call to another page and its return, 1000 times
        movs    r9, #0
        ldr     r5, =1000
1:      bl      thumb_count
        subs    r5, #1
        bne     1b
        mov     r0, r9
        ldr     r1, =1000
        movs    r2, #'a'
        blx     .Lcheck
        @ b: Thumb to ARM (blx imm, bx lr back) and ARM to Thumb (blx reg, pop {pc} back), 1000 times each
        movs    r9, #0
        ldr     r5, =1000
2:      blx     arm_count
        blx     arm_calls_thumb
        subs    r5, #1
        bne     2b
        mov     r0, r9
        ldr     r1, =2000
        movs    r2, #'b'
        blx     .Lcheck
        @ c: a conditional return in an IT block, taken every other time
        movs    r9, #0
        ldr     r5, =1000
3:      and     r0, r5, #1
        bl      thumb_itret
        subs    r5, #1
        bne     3b
        mov     r0, r9
        ldr     r1, =500
        movs    r2, #'c'
        blx     .Lcheck
        @ d: an indirect call to VA_F 100 times, VA_F remapped to code that returns 2 (TLBIMVA), 100 times more
        movs    r6, #0
        ldr     r3, =VA_F + 1
        movs    r5, #100
4:      blx     r3
        adds    r6, r6, r0
        subs    r5, #1
        bne     4b
        ldr     r0, =L2
        ldr     r1, =PA_F2 | PG
        str     r1, [r0, #4]
        dsb
        ldr     r0, =VA_F
        mcr     p15, 0, r0, c8, c7, 1       @ TLBIMVA
        dsb
        isb
        ldr     r3, =VA_F + 1
        movs    r5, #100
5:      blx     r3
        adds    r6, r6, r0
        subs    r5, #1
        bne     5b
        mov     r0, r6
        movs    r1, #300
        movs    r2, #'d'
        blx     .Lcheck
        @ e: VA_R's code calls out 100 times and returns 1; then the callee remaps VA_R under the return address
        movs    r6, #0
        movs    r5, #100
        ldr     r4, =nothing
6:      ldr     r3, =VA_R + 1
        blx     r3
        adds    r6, r6, r0
        subs    r5, #1
        bne     6b
        ldr     r4, =remap_r
        ldr     r3, =VA_R + 1
        blx     r3
        adds    r6, r6, r0
        mov     r0, r6
        movs    r1, #102
        movs    r2, #'e'
        blx     .Lcheck
        @ f: code at one address in both states, called as Thumb 100 times, then as ARM through bx with bit 0 clear
        movs    r6, #0
        movs    r5, #100
        ldr     r3, =dual
        adds    r3, #1
7:      blx     r3
        adds    r6, r6, r0
        subs    r5, #1
        bne     7b
        ldr     r3, =dual
        movs    r1, #0                      @ N clear: the ARM word at dual is a no-op (MI)
        blx     r3
        adds    r6, r6, r0
        mov     r0, r6
        movs    r1, #203
        movs    r2, #'f'
        blx     .Lcheck
        @ g: one physical page at two VAs, called through each 100 times: its mov r0, pc reads the VA it runs at
        movs    r6, #0
        movs    r5, #100
8:      ldr     r3, =VA_P1 + 1
        blx     r3
        ldr     r1, =VA_P1 + 4
        cmp     r0, r1
        it      ne
        addne   r6, #1
        ldr     r3, =VA_P2 + 1
        blx     r3
        ldr     r1, =VA_P2 + 4
        cmp     r0, r1
        it      ne
        addne   r6, #1
        subs    r5, #1
        bne     8b
        mov     r0, r6
        movs    r1, #0
        movs    r2, #'g'
        blx     .Lcheck
        @ h: three return addresses in one jump cache set (page offsets 0x4, 0x80, 0x10c), from calls to another page,
        @ in turn 100 times: each must come back to its own code
        movs    r6, #0
        movs    r5, #100
        ldr     r4, =nothing
        bl      set3
        mov     r0, r6
        ldr     r1, =700
        movs    r2, #'h'
        blx     .Lcheck
        pop     {r4-r8, pc}
        .ltorg

@ blx r4 at page offsets 0x2, 0x7e and 0x10a: returns to 0x4, 0x80 and 0x10c, one set of a two-way jump cache
        .balign 4096
        .thumb_func
set3:   push    {lr}
1:      blx     r4
        adds    r6, #1
        b       2f
        .org    set3 + 0x7e
2:      blx     r4
        adds    r6, #2
        b       3f
        .org    set3 + 0x10a
3:      blx     r4
        adds    r6, #4
        subs    r5, #1
        bne     1b
        pop     {pc}
        .ltorg

@ As Thumb: movs r0, #2; bx lr. As ARM: the same word is strbmi (skipped with N clear), then mov r0, #3; bx lr.
        .balign 4
dual:
        .short  0x2002, 0x4770
        .word   0xe3a00003, 0xe12fff1e
        .ltorg

@ the remapped pages' code (Thumb, position independent)
        .balign 4
f1_code:
        movs    r0, #1
        bx      lr
        .balign 4
f2_code:
        movs    r0, #2
        bx      lr
        .balign 4
r1_code:
        push    {r4, lr}
        blx     r4
        movs    r0, #1
        pop     {r4, pc}
        .balign 4
p_code:
        mov     r0, pc
        bx      lr
        .balign 4
r2_code:
        push    {r4, lr}
        blx     r4
        movs    r0, #2
        pop     {r4, pc}
