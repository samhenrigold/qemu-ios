#ifndef HW_ARM_IT_IBOOT_H
#define HW_ARM_IT_IBOOT_H

/*
 * Board-agnostic pattern matches over a decrypted iBoot image loaded at
 * `base` (the S5L8720 iPod loads it at 0x0ff00000 and jumps in directly;
 * the iPad runs its real iBoot from its own image and needs none of this).
 * Every finder is derived from what the image contains -- a string, the one
 * literal that references it, the Thumb `ldr rN, [pc, #imm]` that loads the
 * literal -- never from a build's offsets. Each returns 0 for absent,
 * ambiguous or out-of-window matches, and tests/ipod/test_iboot_literals.py
 * pins the answer for every iPod touch 2G iBoot (2.1.1 .. 4.2.1).
 */

#include <stddef.h>
#include <stdint.h>

/*
 * The boot security epoch this iBoot demands of SYSIC POWER_ID[31:24] in
 * miu_init ("Epoch Mismatch" otherwise): the floor its epoch helper applies
 * to the chip ID fuse field. The LLB latches that byte on a real boot; a
 * board that jumps straight into iBoot has to synthesise it. 0 = not found.
 */
uint32_t it_iboot_find_epoch(const uint8_t *image, size_t size);

/*
 * The same demand in an armv7 iBoot (the A4's, iBoot-817 .. 1219): miu_init
 * does `bl epoch; cmp.w r0, rN, lsr #24` against PMGR POWER_ID, and epoch()
 * is `bl fuse_field; cmp r0, #N; it <cc>; mov r0, #M`, the fused field
 * floored at the build's epoch. Returns what epoch() computes for `fuse`
 * (the CHIPID field the caller models), which is what LLB latches into
 * POWER_ID[31:24] on hardware; 0 = not found, or found more than once.
 */
uint32_t it_iboot_find_miu_epoch(const uint8_t *image, size_t size,
                                 uint32_t fuse);

/*
 * Guest address of gBootArgs.commandLine, from the literal loads around
 * iBoot's `printf("gBootArgs.commandLine = [%s]\n", ...)`. Only the 2.x
 * iBoots (385.x) load the buffer from a literal; later ones pass it in a
 * register, so this is the NAND-boot (bootrom -> LLB -> iBoot) mechanism.
 */
uint32_t it_iboot_find_command_line(const uint8_t *image, size_t size,
                                    uint32_t base);

#ifndef IT_IBOOT_HOST_TEST
#include "exec/hwaddr.h"
#include "exec/memory.h"

/* it_iboot_find_epoch over the image staged at [base, base + image_size). */
uint32_t it_iboot_epoch(AddressSpace *as, uint32_t base, size_t image_size);
#endif

#endif
