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
 * Offset of the literal word iBoot hands XNU as its kernel command line on a
 * normal boot (the empty string; restore mode uses
 * "rd=md0 nand-enable-reformat=1 -progress" through the word after it).
 * Redirecting that word to a staged string sets the command line before
 * PE_init_platform and AMFI read it.
 */
uint32_t it_iboot_find_boot_args_literal(const uint8_t *image, size_t size,
                                         uint32_t base);

/*
 * The boot security epoch this iBoot demands of SYSIC POWER_ID[31:24] in
 * miu_init ("Epoch Mismatch" otherwise): the floor its epoch helper applies
 * to the chip ID fuse field. The LLB latches that byte on a real boot; a
 * board that jumps straight into iBoot has to synthesise it. 0 = not found.
 */
uint32_t it_iboot_find_epoch(const uint8_t *image, size_t size);

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

/*
 * Stage `args` at `staging` and point the normal-boot literal of the iBoot
 * image at [base, base + image_size) in guest memory at it. Returns the guest
 * address of the redirected literal, 0 if the image is not recognised (and
 * then nothing is written).
 */
uint32_t it_iboot_inject_boot_args(AddressSpace *as, uint32_t base,
                                   size_t image_size, const char *args,
                                   hwaddr staging);

/* it_iboot_find_epoch over the image staged at [base, base + image_size). */
uint32_t it_iboot_epoch(AddressSpace *as, uint32_t base, size_t image_size);
#endif

#endif
