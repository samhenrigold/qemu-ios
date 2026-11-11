/* Both generations feed the same decoder command parser.
 * SLICE hw/arm/s5l8920_vxd.c define VXD_BS_SEGS
 * SLICE hw/arm/s5l8920_vxd.c typedef VXDRender
 * SLICE hw/arm/s5l8920_vxd.c define CMD_|VEC_H264_FE_SPS0|LLDMA_PERIPH_SHIFTREG|LLDMA_LAST|CMDS_
 * SLICE hw/arm/s5l8920_vxd.c fn vxd_parse_commands vxd_parse_render vxd_parse_packed
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
static uint32_t mem[4096];
static bool vxd_rw(uint32_t ptd, uint32_t va, void *buf, size_t len, bool write)
{
    if (ptd != 0x123000 || va > sizeof(mem) || len > sizeof(mem) - va || write)
        return false;
    memcpy(buf, (char *)mem + va, len);
    return true;
}
#include "slice.h"
int main(void)
{
    uint32_t commands[] = {CMD_REGVALPAIR_WRITE << 28 | 1,
                           VEC_H264_FE_SPS0,
                           0xfeed,
                           CMD_RENDEC_WRITE << 28 | 6,
                           (9 << 16) | (0x1000 << 2) | 3,
                           0x02cf04ff,
                           0,
                           0x10211000,
                           0x600000,
                           0x700000,
                           CMD_SR_SETUP << 28,
                           7,
                           0,
                           CMD_RENDEC_WRITE << 28 | 2,
                           (1 << 16) | (0x1040 << 2) | 3,
                           1920,
                           CMD_LLDMA << 28 | (0x800 >> 4),
                           CMD_COMPLETION << 28};
    memcpy((char *)mem + 0x100, commands, sizeof(commands));
    mem[0x800 / 4 + 1] = 231;
    mem[0x800 / 4 + 2] = LLDMA_PERIPH_SHIFTREG;
    mem[0x800 / 4 + 6] = 0x900000;
    mem[0x900 / 4 + 1] = G_N_ELEMENTS(commands);
    mem[0x900 / 4 + 2] = 0x350;
    mem[0x900 / 4 + 6] = 0x100;
    VXDRender a = {0}, b = {0};
    assert(
        vxd_parse_commands(0x123000, 0x100, G_N_ELEMENTS(commands), &a, false, false));
    assert(vxd_parse_render(0x123000, 0x900, &b, false));
    assert(a.row_stride == 1920);
    assert(!memcmp(&a, &b, sizeof(a)));
    assert(a.fe[0] == 0xfeed && a.sr_offset == 7);
    assert(a.size == 0x02cf04ff && a.mode == 0x10211000);
    assert(a.luma == 0x600000 && a.chroma == 0x700000);
    assert(a.bs_va == 0x900000 && a.bs_len == 231);
    uint32_t a6[] = {CMD_HEADER_EXT << 28,
                     0x1009,
                     0,
                     0,
                     0,
                     0,
                     0,
                     0,
                     CMD_REGVALPAIR_WRITE << 28 | 1,
                     VEC_H264_FE_SPS0,
                     0xfeed,
                     CMD_RENDEC_INLINE << 28 | 5 << 16 | (0x1000 << 2),
                     0x02cf04ff,
                     0,
                     0x10211000,
                     0x600000,
                     0x700000,
                     CMD_SR_SETUP << 28,
                     7,
                     0,
                     CMD_RENDEC_INLINE << 28 | 1 << 16 | (0x1040 << 2),
                     1920,
                     CMD_LLDMA << 28 | (0x800 >> 4),
                     CMD_COMPLETION << 28};
    memcpy((char *)mem + 0x100, a6, sizeof(a6));
    mem[0x900 / 4 + 1] = G_N_ELEMENTS(a6);
    assert(vxd_parse_render(0x123000, 0x900, &b, true));
    assert(!memcmp(&a, &b, sizeof(a)));
    /* AppleVXD375's boards ("a6" off) step over the commands only AppleVXD390 sends, as they always did */
    VXDRender old = {0};
    vxd_parse_render(0x123000, 0x900, &old, false);
    assert(memcmp(&a, &old, sizeof(a)));
    uint32_t packed[] = {CMD_HEADER_EXT << 28,
                         0x1009,
                         0,
                         0,
                         0,
                         0,
                         0,
                         0,
                         0xe0000410,
                         0x2000,
                         CMD_REGVALPAIR_WRITE << 28 | 1 << 16 | 0x0a00,
                         0xfeed,
                         CMD_RENDEC_INLINE << 28 | 5 << 16 | 0x1000,
                         0x02cf04ff,
                         0,
                         0x10211000,
                         0x600000,
                         0x700000,
                         CMD_SR_SETUP << 28,
                         7,
                         0,
                         CMD_LLDMA << 28 | 231,
                         0x900000,
                         CMD_RENDEC_INLINE << 28 | 1 << 16 | 0x1040,
                         1920,
                         CMD_COMPLETION << 28};
    memcpy((char *)mem + 0x200, packed, sizeof(packed));
    assert(vxd_parse_packed(0x123000, 0x200, G_N_ELEMENTS(packed), &b));
    assert(!memcmp(&a, &b, sizeof(a)));
    assert(!vxd_parse_packed(0x123000, 0x200, 9,
                             &b)); /* incomplete context transfer */
    assert(!vxd_parse_packed(0x123000, 0x200, 11,
                             &b)); /* incomplete register write */
    assert(!vxd_parse_packed(0x123000, 0x200, 22,
                             &b)); /* incomplete bitstream address */
    assert(!vxd_parse_packed(0x123000, 0x900, 8,
                             &b)); /* an LLDMA is not a packed header */
    assert(!vxd_parse_commands(0x123000, 0x100, 7, &b,
                               false, true)); /* truncated extended header */
    assert(!vxd_parse_commands(0x123000, 0x100, 13, &b,
                               false, true)); /* truncated inline registers */
    assert(!vxd_parse_commands(0x123000, 0x100, 0, &a, false, false));
    assert(!vxd_parse_commands(0x123000, 0x100, 0x10000, &a, false, false));
    assert(!vxd_parse_commands(0x999000, 0x100, 5, &a, false, false));
    assert(!vxd_parse_commands(0x123000, sizeof(mem) - 4, 5, &a, false, false));
    mem[0x900 / 4 + 2] = 0;
    assert(!vxd_parse_render(0x123000, 0x900, &a, false));
    puts(
        "ok: legacy, A6 LLDMA and packed DEVA command streams, extended stride");
}
