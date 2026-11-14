@ ASID, TLB and jump-cache checks for tests/ipod/tcg-asid-test.sh: a bare-metal program run as the bootrom (at 0, SVC
@ mode, MMU off) on an ARMv6 or ARMv7 board whose RAM starts at RAM (-DRAM=). It maps a few test pages at 0x70000000
@ in two L1 tables (odd ASIDs use T1, even ones T2) beside identity sections for itself and the RAM, and does what a
@ kernel does to them: switch ASID, remap, invalidate, rewrite code. One character per check on the semihosting
@ console: its letter in lowercase if it passed, uppercase if not; then 'E', and it exits. An exception hangs it.
        .syntax unified
        .arm
        .text
        .equ T1, RAM + 0x20000          @ L1 table of the odd ASIDs
        .equ T2, RAM + 0x24000          @ L1 table of the even ASIDs
        .equ L2A, RAM + 0x28000         @ T1's L2 table for VA 0x70000000-0x700fffff
        .equ L2B, RAM + 0x28400         @ T2's
        .equ PA_A, RAM + 0x30000        @ data 0xa1, VA_D in T1 (nG)
        .equ PA_B, RAM + 0x31000        @ data 0xb2, VA_D in T2 (nG)
        .equ PA_C1, RAM + 0x32000       @ code: return 1, VA_C in T1 (nG)
        .equ PA_C2, RAM + 0x33000       @ code: return 2, VA_C in T2 (nG)
        .equ PA_C3, RAM + 0x34000       @ code: return 3
        .equ PA_G1, RAM + 0x35000       @ data 0xc3, VA_G in both (global)
        .equ PA_G2, RAM + 0x36000       @ data 0xd4
        .equ PA_U1, RAM + 0x37000       @ data 0xe5, VA_U in T1 (nG, read unprivileged)
        .equ PA_U2, RAM + 0x38000       @ data 0xf6, VA_U in T2 (nG)
        .equ PA_U3, RAM + 0x39000       @ data 0x17
        .equ PA_X, RAM + 0x3a000        @ code written through VA_X in T1 (nG), run through the identity map
        .equ PA_S1, RAM + 0x400000      @ 1M sections: data 0x31 at +0 and +0x5000
        .equ PA_S2, RAM + 0x500000      @ 0x42
        .equ PA_S3, RAM + 0x600000      @ 0x53
        .equ VA_D, 0x70000000
        .equ VA_C, 0x70001000
        .equ VA_G, 0x70002000
        .equ VA_U, 0x70003000
        .equ VA_X, 0x70004000
        .equ VA_S1, 0x71000000    @ global section in both tables
        .equ VA_S2, 0x71100000    @ the same
        .equ VA_NS, 0x72000000    @ non-global section: PA_S1 in T1, PA_S2 in T2
        .equ VA_N, 0x70010000     @ 40 nG pages: word i in T1, 0x100 + i in T2
        .equ NPAGES, 40
        .equ PA_N1, RAM + 0x100000      @ the 40 pages in T1
        .equ PA_N2, RAM + 0x200000      @ in T2
        .equ PG_NG, 0x872         @ small page: nG, AP=11, TEX=001 (normal), type 2
        .equ PG_G, 0x072          @ the same, global
        .equ SECT, 0x1c02         @ section: AP=11, TEX=001, domain 0, global
        .equ SECT_NG, 0x21c02     @ the same, nG

_start:
        cpsid   aif
        ldr     sp, =RAM + 0xf000
        mov     r10, #0
        @ L1 tables: zero, then identity sections and the L2 tables at VA_D
        ldr     r0, =T1
        mov     r1, #0
        ldr     r2, =0x8000                 @ T1 and T2
2:      str     r1, [r0], #4
        subs    r2, r2, #4
        bne     2b
        ldr     r0, =T1
        bl      .Lsections
        ldr     r0, =T2
        bl      .Lsections
        ldr     r0, =T1 + (VA_D >> 20) * 4
        ldr     r1, =L2A | 1
        str     r1, [r0]
        ldr     r0, =T2 + (VA_D >> 20) * 4
        ldr     r1, =L2B | 1
        str     r1, [r0]
        @ L2 tables
        ldr     r0, =L2A
        mov     r1, #0
        mov     r2, #0x800
3:      str     r1, [r0], #4
        subs    r2, r2, #4
        bne     3b
        ldr     r0, =L2A
        ldr     r1, =PA_A | PG_NG
        str     r1, [r0, #0]
        ldr     r1, =PA_C1 | PG_NG
        str     r1, [r0, #4]
        ldr     r1, =PA_G1 | PG_G
        str     r1, [r0, #8]
        ldr     r1, =PA_U1 | PG_NG
        str     r1, [r0, #12]
        ldr     r1, =PA_X | PG_NG
        str     r1, [r0, #16]
        ldr     r0, =L2B
        ldr     r1, =PA_B | PG_NG
        str     r1, [r0, #0]
        ldr     r1, =PA_C2 | PG_NG
        str     r1, [r0, #4]
        ldr     r1, =PA_G1 | PG_G
        str     r1, [r0, #8]
        ldr     r1, =PA_U2 | PG_NG
        str     r1, [r0, #12]
        mov     r3, #0
4:      ldr     r0, =L2A + 0x10 * 4
        ldr     r1, =PA_N1 | PG_NG
        add     r1, r1, r3, lsl #12
        str     r1, [r0, r3, lsl #2]
        ldr     r2, =PA_N1
        str     r3, [r2, r3, lsl #12]       @ word i at its T1 page
        ldr     r0, =L2B + 0x10 * 4
        ldr     r1, =PA_N2 | PG_NG
        add     r1, r1, r3, lsl #12
        str     r1, [r0, r3, lsl #2]
        ldr     r2, =PA_N2
        add     r1, r3, #0x100
        str     r1, [r2, r3, lsl #12]       @ word 0x100 + i at its T2 page
        add     r3, r3, #1
        cmp     r3, #NPAGES
        bne     4b
        @ data and code pages
        ldr     r0, =PA_A
        mov     r1, #0xa1
        str     r1, [r0]
        ldr     r0, =PA_B
        mov     r1, #0xb2
        str     r1, [r0]
        ldr     r0, =PA_G1
        mov     r1, #0xc3
        str     r1, [r0]
        ldr     r0, =PA_G2
        mov     r1, #0xd4
        str     r1, [r0]
        ldr     r0, =PA_U1
        mov     r1, #0xe5
        str     r1, [r0]
        ldr     r0, =PA_U2
        mov     r1, #0xf6
        str     r1, [r0]
        ldr     r0, =PA_U3
        mov     r1, #0x17
        str     r1, [r0]
        mov     r12, #0x5000                @ the second page read in each section
        ldr     r0, =PA_S1
        mov     r1, #0x31
        str     r1, [r0]
        str     r1, [r0, r12]
        ldr     r0, =PA_S2
        mov     r1, #0x42
        str     r1, [r0]
        str     r1, [r0, r12]
        ldr     r0, =PA_S3
        mov     r1, #0x53
        str     r1, [r0]
        str     r1, [r0, r12]
        ldr     r1, =PA_S1 | SECT
        ldr     r2, =VA_S1
        bl      .Lsect2
        ldr     r1, =PA_S2 | SECT
        ldr     r2, =VA_S2
        bl      .Lsect2
        ldr     r0, =T1 + (VA_NS >> 20) * 4
        ldr     r1, =PA_S1 | SECT_NG
        str     r1, [r0]
        ldr     r0, =T2 + (VA_NS >> 20) * 4
        ldr     r1, =PA_S2 | SECT_NG
        str     r1, [r0]
        ldr     r0, =PA_C1
        mov     r1, #1
        bl      .Lputcode
        ldr     r0, =PA_C2
        mov     r1, #2
        bl      .Lputcode
        ldr     r0, =PA_C3
        mov     r1, #3
        bl      .Lputcode
        @ MMU on: domain 0 client, TTBCR 0, ASID 1 on T1
        mov     r0, #1
        mcr     p15, 0, r0, c3, c0, 0       @ DACR
        mov     r0, #0
        mcr     p15, 0, r0, c2, c0, 2       @ TTBCR
        mov     r0, #1
        bl      .Lswitch
        mcr     p15, 0, r0, c8, c7, 0       @ TLBIALL
        mrc     p15, 0, r0, c1, c0, 0
        orr     r0, r0, #1                  @ M
        orr     r0, r0, #0x800000           @ XP (v6; RES1 on v7)
        bic     r0, r0, #0x30000000         @ no TRE/AFE
        mcr     p15, 0, r0, c1, c0, 0
        mcr     p15, 0, r0, c7, c5, 4       @ ISB

        @ a-c: a kernel load from a non-global page follows the ASID switch
        ldr     r0, =VA_D
        ldr     r0, [r0]
        mov     r1, #0xa1
        mov     r2, #'a'
        bl      .Lcheck
        mov     r0, #2
        bl      .Lswitch
        ldr     r0, =VA_D
        ldr     r0, [r0]
        mov     r1, #0xb2
        mov     r2, #'b'
        bl      .Lcheck
        mov     r0, #1
        bl      .Lswitch
        ldr     r0, =VA_D
        ldr     r0, [r0]
        mov     r1, #0xa1
        mov     r2, #'c'
        bl      .Lcheck
        @ d-e: more non-global kernel pages than are tracked one by one
        mov     r2, #0
        bl      .Lreadpages                 @ ASID 1: fills them
        mov     r0, #2
        bl      .Lswitch
        mov     r1, #0x100
        mov     r2, #'d'
        bl      .Lreadpages
        mov     r0, #1
        bl      .Lswitch
        mov     r1, #0
        mov     r2, #'e'
        bl      .Lreadpages
        @ f-h: code at one VA in two address spaces, reached by an indirect branch
        bl      .Lcallc
        mov     r1, #1
        mov     r2, #'f'
        bl      .Lcheck
        mov     r0, #2
        bl      .Lswitch
        bl      .Lcallc
        mov     r1, #2
        mov     r2, #'g'
        bl      .Lcheck
        mov     r0, #1
        bl      .Lswitch
        bl      .Lcallc
        mov     r1, #1
        mov     r2, #'h'
        bl      .Lcheck
        @ i: ASID 1's code page remapped while ASID 2 runs, invalidated by MVA and ASID
        mov     r0, #2
        bl      .Lswitch
        bl      .Lcallc
        ldr     r0, =L2A
        ldr     r1, =PA_C3 | PG_NG
        str     r1, [r0, #4]
        ldr     r0, =VA_C | 1
        mcr     p15, 0, r0, c8, c7, 1       @ TLBIMVA
        mov     r0, #1
        bl      .Lswitch
        bl      .Lcallc
        mov     r1, #3
        mov     r2, #'i'
        bl      .Lcheck
        @ j: remapped again while ASID 2 runs, invalidated by ASID
        mov     r0, #2
        bl      .Lswitch
        bl      .Lcallc
        ldr     r0, =L2A
        ldr     r1, =PA_C1 | PG_NG
        str     r1, [r0, #4]
        mov     r0, #1
        mcr     p15, 0, r0, c8, c7, 2       @ TLBIASID 1
        mov     r0, #1
        bl      .Lswitch
        bl      .Lcallc
        mov     r1, #1
        mov     r2, #'j'
        bl      .Lcheck
        @ k: the current ASID's code remapped and invalidated by ASID; l: back, by MVA
        ldr     r0, =L2A
        ldr     r1, =PA_C3 | PG_NG
        str     r1, [r0, #4]
        mov     r0, #1
        mcr     p15, 0, r0, c8, c7, 2       @ TLBIASID 1
        bl      .Lcallc
        mov     r1, #3
        mov     r2, #'k'
        bl      .Lcheck
        ldr     r0, =L2A
        ldr     r1, =PA_C1 | PG_NG
        str     r1, [r0, #4]
        ldr     r0, =VA_C | 1
        mcr     p15, 0, r0, c8, c7, 1       @ TLBIMVA
        bl      .Lcallc
        mov     r1, #1
        mov     r2, #'l'
        bl      .Lcheck
        @ m: more address spaces than jump caches (65 for 64), twice round: odd ASIDs return 1, even 2
        mov     r9, #0                      @ mismatches
        mov     r8, #130
        mov     r7, #1                      @ ASID
5:      mov     r0, r7
        bl      .Lswitch
        bl      .Lcallc
        and     r1, r7, #1
        rsb     r1, r1, #2
        cmp     r0, r1
        addne   r9, r9, #1
        add     r7, r7, #1
        cmp     r7, #66
        moveq   r7, #1
        subs    r8, r8, #1
        bne     5b
        mov     r0, r9
        mov     r1, #0
        mov     r2, #'m'
        bl      .Lcheck
        @ n-q: a global kernel mapping kept across switches, then remapped
        mov     r0, #1
        bl      .Lswitch
        ldr     r0, =VA_G
        ldr     r0, [r0]
        mov     r1, #0xc3
        mov     r2, #'n'
        bl      .Lcheck
        mov     r0, #2
        bl      .Lswitch
        ldr     r0, =VA_G
        ldr     r0, [r0]                    @ the global entry, if kept
        ldr     r0, =L2A
        ldr     r1, =PA_G2 | PG_G
        str     r1, [r0, #8]
        ldr     r0, =L2B
        str     r1, [r0, #8]
        ldr     r0, =VA_G | 2
        mcr     p15, 0, r0, c8, c7, 1       @ TLBIMVA (a global entry goes whatever the ASID)
        ldr     r0, =VA_G
        ldr     r0, [r0]
        mov     r1, #0xd4
        mov     r2, #'o'
        bl      .Lcheck
        mov     r0, #1
        bl      .Lswitch
        ldr     r0, =VA_G
        ldr     r0, [r0]
        mov     r1, #0xd4
        mov     r2, #'p'
        bl      .Lcheck
        ldr     r0, =L2A
        ldr     r1, =PA_G1 | PG_G
        str     r1, [r0, #8]
        ldr     r0, =L2B
        str     r1, [r0, #8]
        mcr     p15, 0, r0, c8, c7, 0       @ TLBIALL
        ldr     r0, =VA_G
        ldr     r0, [r0]
        mov     r1, #0xc3
        mov     r2, #'q'
        bl      .Lcheck
        @ r-t: code rewritten (a JIT): through another mapping while another ASID runs, through
        @ another mapping, and through its own
        bl      .Lcallc
        mov     r0, #2
        bl      .Lswitch
        bl      .Lcallc
        ldr     r0, =PA_C1
        mov     r1, #4
        bl      .Lputcode
        mov     r0, #1
        bl      .Lswitch
        bl      .Lcallc
        mov     r1, #4
        mov     r2, #'r'
        bl      .Lcheck
        ldr     r0, =PA_C1
        mov     r1, #5
        bl      .Lputcode
        bl      .Lcallc
        mov     r1, #5
        mov     r2, #'s'
        bl      .Lcheck
        ldr     r0, =VA_C
        mov     r1, #6
        bl      .Lputcode
        bl      .Lcallc
        mov     r1, #6
        mov     r2, #'t'
        bl      .Lcheck
        @ u: ASID 1's code remapped while ASID 2 runs, then everything invalidated
        mov     r0, #2
        bl      .Lswitch
        bl      .Lcallc
        ldr     r0, =L2A
        ldr     r1, =PA_C3 | PG_NG
        str     r1, [r0, #4]
        mcr     p15, 0, r0, c8, c7, 0       @ TLBIALL
        mov     r0, #1
        bl      .Lswitch
        bl      .Lcallc
        mov     r1, #3
        mov     r2, #'u'
        bl      .Lcheck

        @ v-z, 1: user (PL0) entries, read and written unprivileged (LDRT/STRT): TCG keeps the ASID left's and brings
        @ them back when it is switched straight back to, as a tagged TLB keeps them
        @ v: each ASID reads its own page
        mov     r0, #1
        bl      .Lswitch
        ldr     r4, =VA_U
        ldrt    r0, [r4]
        mov     r0, #2
        bl      .Lswitch
        ldrt    r0, [r4]
        mov     r1, #0xf6
        mov     r2, #'v'
        bl      .Lcheck
        @ w: ASID 1's page remapped with no TLBI while ASID 2 runs: back in ASID 1 its entry still reads the old page
        ldr     r0, =L2A
        ldr     r1, =PA_U3 | PG_NG
        str     r1, [r0, #12]
        mov     r0, #1
        bl      .Lswitch
        ldrt    r0, [r4]
        mov     r1, #0xe5
        mov     r2, #'w'
        bl      .Lcheck
        @ x: the kept entry goes with a TLBIMVA made while ASID 2 runs
        mov     r0, #2
        bl      .Lswitch
        ldrt    r0, [r4]
        ldr     r0, =VA_U | 1
        mcr     p15, 0, r0, c8, c7, 1       @ TLBIMVA
        mov     r0, #1
        bl      .Lswitch
        ldrt    r0, [r4]
        mov     r1, #0x17
        mov     r2, #'x'
        bl      .Lcheck
        @ y: and with a TLBIASID of ASID 1
        mov     r0, #2
        bl      .Lswitch
        ldrt    r0, [r4]
        ldr     r0, =L2A
        ldr     r1, =PA_U1 | PG_NG
        str     r1, [r0, #12]
        mov     r0, #1
        mcr     p15, 0, r0, c8, c7, 2       @ TLBIASID 1
        mov     r0, #1
        bl      .Lswitch
        ldrt    r0, [r4]
        mov     r1, #0xe5
        mov     r2, #'y'
        bl      .Lcheck
        @ z: and with a TLBIALL
        mov     r0, #2
        bl      .Lswitch
        ldrt    r0, [r4]
        ldr     r0, =L2A
        ldr     r1, =PA_U3 | PG_NG
        str     r1, [r0, #12]
        mcr     p15, 0, r0, c8, c7, 0       @ TLBIALL
        mov     r0, #1
        bl      .Lswitch
        ldrt    r0, [r4]
        mov     r1, #0x17
        mov     r2, #'z'
        bl      .Lcheck
        @ 1: a kept writable entry still catches code written through it: ASID 1 writes PA_X's code through VA_X, ASID 2
        @ runs it (so its TB is there), ASID 1 rewrites it through the kept entry, and the code run is the new one
        ldr     r0, =VA_X
        mov     r1, #7
        bl      .Lputcode_t
        mov     r0, #2
        bl      .Lswitch
        ldr     r4, =PA_X
        blx     r4
        mov     r0, #1
        bl      .Lswitch
        ldr     r0, =VA_X
        mov     r1, #8
        bl      .Lputcode_t
        ldr     r4, =PA_X
        blx     r4
        mov     r1, #8
        mov     r2, #'1'
        bl      .Lcheck
        @ 2-5: 1M sections, which the TLB holds as one entry per 4K page touched
        mov     r12, #0x5000
        @ 2: a TLBIMVA anywhere in a section takes every page of it
        ldr     r4, =VA_S1
        ldr     r0, [r4]
        ldr     r0, [r4, r12]
        ldr     r1, =PA_S3 | SECT
        ldr     r2, =VA_S1
        bl      .Lsect2
        ldr     r0, =(VA_S1 + 0x9000) | 1
        mcr     p15, 0, r0, c8, c7, 1       @ TLBIMVA
        ldr     r5, [r4]
        ldr     r6, [r4, r12]
        add     r0, r5, r6
        mov     r1, #0xa6
        mov     r2, #'2'
        bl      .Lcheck
        @ 3: a TLBIMVA in one section leaves another's entries (remapped without TLBI: still the old page)
        ldr     r4, =VA_S2
        ldr     r0, [r4]
        ldr     r1, =PA_S3 | SECT
        ldr     r2, =VA_S2
        bl      .Lsect2
        ldr     r0, =VA_S1 | 1
        mcr     p15, 0, r0, c8, c7, 1       @ TLBIMVA
        ldr     r0, [r4]
        mov     r1, #0x42
        mov     r2, #'3'
        bl      .Lcheck
        mcr     p15, 0, r0, c8, c7, 0       @ TLBIALL
        @ 4: a non-global section the kernel read goes at an ASID switch, every page of it
        mov     r0, #1
        bl      .Lswitch
        ldr     r4, =VA_NS
        ldr     r0, [r4]
        ldr     r0, [r4, r12]
        mov     r0, #2
        bl      .Lswitch
        ldr     r5, [r4]
        ldr     r6, [r4, r12]
        add     r0, r5, r6
        mov     r1, #0x84
        mov     r2, #'4'
        bl      .Lcheck
        @ 5: and only it: a global section's entry stays (remapped without TLBI: still the old page)
        ldr     r7, =VA_S2
        ldr     r0, [r7]
        ldr     r0, [r4]
        ldr     r1, =PA_S1 | SECT
        ldr     r2, =VA_S2
        bl      .Lsect2
        mov     r0, #1
        bl      .Lswitch
        ldr     r0, [r7]
        mov     r1, #0x53
        mov     r2, #'5'
        bl      .Lcheck

        mov     r0, #'E'
        bl      .Lputc
        mov     r0, #'\n'
        bl      .Lputc
        mov     r0, #0x18                   @ SYS_EXIT
        ldr     r1, =0x20026                @ ADP_Stopped_ApplicationExit
        svc     0x123456
        b       .

@ r0 = L1 table: identity sections for the ROM at 0 (this code) and 64M of RAM
.Lsections:
        ldr     r1, =SECT
        str     r1, [r0]
        ldr     r1, =RAM | SECT
        add     r0, r0, #(RAM >> 20) * 4
        mov     r2, #64
1:      str     r1, [r0], #4
        add     r1, r1, #0x100000
        subs    r2, r2, #1
        bne     1b
        bx      lr

@ r1 = section descriptor for VA r2 in both tables (no TLBI)
.Lsect2:
        ldr     r0, =T1
        str     r1, [r0, r2, lsr #18]
        ldr     r0, =T2
        str     r1, [r0, r2, lsr #18]
        bx      lr

@ r0 = ASID: TTBR0 = T1 (odd) or T2 (even), then CONTEXTIDR
.Lswitch:
        tst     r0, #1
        ldrne   r1, =T1
        ldreq   r1, =T2
        mcr     p15, 0, r1, c2, c0, 0       @ TTBR0
        mcr     p15, 0, r0, c13, c0, 1      @ CONTEXTIDR
        mcr     p15, 0, r0, c7, c5, 4       @ ISB
        bx      lr

@ call the code at VA_C three times (an indirect branch each), result in r0
.Lcallc:
        push    {r4, r5, lr}
        mov     r5, #3
1:      ldr     r4, =VA_C
        blx     r4
        subs    r5, r5, #1
        bne     1b
        pop     {r4, r5, pc}

@ r0 = address: "mov r0, #r1; bx lr" there, then make it visible to instruction fetch
.Lputcode:
        orr     r1, r1, #0xe3000000
        orr     r1, r1, #0x00a00000
        str     r1, [r0]
        ldr     r1, =0xe12fff1e
        str     r1, [r0, #4]
        mov     r1, #0
        mcr     p15, 0, r1, c7, c10, 4      @ DSB
        mcr     p15, 0, r1, c7, c5, 0       @ ICIALLU
        mcr     p15, 0, r1, c7, c5, 6       @ BPIALL
        mcr     p15, 0, r1, c7, c5, 4       @ ISB
        bx      lr

@ .Lputcode, written unprivileged (STRT)
.Lputcode_t:
        orr     r1, r1, #0xe3000000
        orr     r1, r1, #0x00a00000
        strt    r1, [r0], #4
        ldr     r1, =0xe12fff1e
        strt    r1, [r0]
        mov     r1, #0
        mcr     p15, 0, r1, c7, c10, 4      @ DSB
        mcr     p15, 0, r1, c7, c5, 0       @ ICIALLU
        mcr     p15, 0, r1, c7, c5, 6       @ BPIALL
        mcr     p15, 0, r1, c7, c5, 4       @ ISB
        bx      lr

@ read the NPAGES non-global pages; with r2 != 0, check page i holds r1 + i (one character, r2)
.Lreadpages:
        push    {r4, r5, r6, lr}
        mov     r4, #0
        mov     r6, #0                      @ mismatches
        ldr     r5, =VA_N
1:      ldr     r0, [r5, r4, lsl #12]
        add     r3, r1, r4
        cmp     r0, r3
        addne   r6, r6, #1
        add     r4, r4, #1
        cmp     r4, #NPAGES
        bne     1b
        cmp     r2, #0
        popeq   {r4, r5, r6, pc}
        mov     r0, r6
        mov     r1, #0
        bl      .Lcheck
        pop     {r4, r5, r6, pc}

@ r0 = got, r1 = expected, r2 = the check's letter
.Lcheck:
        cmp     r0, r1
        subne   r2, r2, #0x20
        addne   r10, r10, #1
        mov     r0, r2
@ r0 = character, to the semihosting console (SYS_WRITEC)
.Lputc:
        push    {r0, r1, r2, r3, r12, lr}
        mov     r1, sp
        mov     r0, #3
        svc     0x123456
        pop     {r0, r1, r2, r3, r12, pc}

        .ltorg
