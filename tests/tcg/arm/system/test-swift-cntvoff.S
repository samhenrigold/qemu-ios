/* SPDX-License-Identifier: GPL-2.0-or-later
 * Swift's EL1 CNTVOFF alias must update the enabled virtual timer immediately.
 * Run on versatilepb with -cpu apple-swift and semihosting enabled.
 */
.syntax unified
.arch armv7-a
.arm
.text
.global __start
__start:
    cpsid if
    ldr sp, =0x007ff000
    mov r0, #0
    mov r1, #0
    mcrr p15, 4, r0, r1, c14 /* CNTVOFF = 0 */
    isb
    mrrc p15, 1, r2, r3, c14 /* CNTVCT */
    ldr r4, =50000000
    adds r2, r2, r4
    adc r3, r3, #0
    mcrr p15, 3, r2, r3, c14 /* CNTV_CVAL: comfortably in the future */
    mov r0, #1
    mcr p15, 0, r0, c14, c3, 1 /* CNTV_CTL.ENABLE */
    isb
    mrc p15, 0, r0, c14, c3, 1
    tst r0, #4
    bne fail_initial

    /* Advance virtual time past CVAL without waiting for its old deadline. */
    ldr r0, =-100000000
    mvn r1, #0
    mcrr p15, 4, r0, r1, c14
    isb
    mrc p15, 0, r0, c14, c3, 1
    tst r0, #4
    beq fail_expiry

    /* Restore the offset: the enabled timer must become unexpired again. */
    mov r0, #0
    mov r1, #0
    mcrr p15, 4, r0, r1, c14
    isb
    mrc p15, 0, r0, c14, c3, 1
    tst r0, #4
    bne fail_clear
    adr r1, pass_message
    mov r0, #4
    svc #0x123456
    ldr r1, =0x20026
    mov r0, #0x18
    svc #0x123456
fail_initial:
    adr r1, initial_message
    b fail
fail_expiry:
    adr r1, expiry_message
    b fail
fail_clear:
    adr r1, clear_message
fail:
    mov r0, #4
    svc #0x123456
    ldr r1, =0x20023
    mov r0, #0x18
    svc #0x123456
    b .
.balign 4
pass_message: .asciz "PASS: Swift CNTVOFF updates virtual timer expiry and clear\n"
initial_message: .asciz "FAIL: virtual timer initially expired\n"
expiry_message: .asciz "FAIL: CNTVOFF did not expire the virtual timer\n"
clear_message: .asciz "FAIL: CNTVOFF did not clear virtual timer expiry\n"
