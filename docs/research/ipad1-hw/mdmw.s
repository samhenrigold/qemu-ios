@ Two iBEC console commands for poking the real iPad over irecovery's shell:
@   md <addr> [count]   print count words from addr
@   mw <addr> <value>   write a word, read it back
@ Handlers get (argc, argv); iBoot cmd_arg is 0x14 bytes with .u at +4.
@ printf is iBEC-817.29 (7B500) 0x5ff124f0 (Thumb).
.syntax unified
.thumb
md: push {r4-r6, lr}
    ldr r4, [r1, #0x18]
    movs r5, #1
    cmp r0, #3
    blt 1f
    ldr r5, [r1, #0x2c]
1:  ldr r6, =0x5ff124f1
2:  ldr r2, [r4]
    mov r1, r4
    adr r0, fmt
    blx r6
    adds r4, #4
    subs r5, #1
    bne 2b
    movs r0, #0
    pop {r4-r6, pc}
mw: push {r4, lr}
    cmp r0, #3
    blt 3f
    ldr r4, [r1, #0x18]
    ldr r2, [r1, #0x2c]
    str r2, [r4]
    dsb sy
    ldr r2, [r4]
    mov r1, r4
    adr r0, fmt
    ldr r3, =0x5ff124f1
    blx r3
3:  movs r0, #0
    pop {r4, pc}
.align 2
fmt: .asciz "%08x: %08x\n"
.ltorg
