/* H5P UI scanout at IOVA zero, with scattered pages and independent layers.
 * Native 11A465 programs UI+90=1 and UI+98=page-table PA; UI+44 may be zero.
 * Mutations: reject base zero; bypass translation; share UI0's table with UI1;
 * read across a physical page boundary without walking the next PTE.
 *
 * SLICE:types hw/arm/s5l8930_display.c define DP_
 * SLICE:types hw/arm/s5l8930_display.c typedef UILayer
 * SLICE hw/arm/s5l8930_display.c fn h5p_ui_xlate fb_read scanout_layer layer_row_bytes layer_fetch
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MEMTXATTRS_UNSPECIFIED 0
typedef uint64_t hwaddr;
typedef struct {
    uint32_t regs[0x6000 / 4];
} DisplayPipe;
typedef struct {
    bool h5p;
    DisplayPipe pipe[1];
} S5L8930DisplayState;
static uint8_t ram[0x10000];
static int address_space_memory;
static uint32_t address_space_ldl_le(int *as, hwaddr pa, int attrs, void *res)
{
    uint32_t value;
    assert(pa + 4 <= sizeof(ram));
    memcpy(&value, ram + pa, 4);
    return value;
}
static void address_space_read(int *as, hwaddr pa, int attrs, void *dst,
                               unsigned len)
{
    assert(pa + len <= sizeof(ram));
    memcpy(dst, ram + pa, len);
}
static hwaddr dart_xlate(S5L8930DisplayState *s, uint32_t va)
{
    return va;
}
#include "types.h"
#include "slice.h"

static void put(uint32_t pa, uint32_t value)
{
    memcpy(ram + pa, &value, 4);
}

int main(void)
{
    S5L8930DisplayState s = {.h5p = true};
    UILayer u = {0};
    enum { W = 32, H = 48, BYTES = W * H * 4 };
    uint8_t out[BYTES];
    for (unsigned layer = 0; layer < 2; layer++) {
        uint32_t *ui = s.pipe[0].regs + (0x4000 + layer * 0x1000) / 4;
        uint32_t table = 0x1000 + layer * 0x1000;
        uint32_t first = 0x8000 + layer * 0x2000,
                 second = 0x3000 + layer * 0x2000;
        put(table, first | 1);
        put(table + 4, second | 1);
        for (unsigned i = 0; i < BYTES; i++) {
            ram[(i < 4096 ? first : second) + (i & 4095)] =
                (i * 7 + layer * 53) & 255;
        }
        s.pipe[0].regs[DP_LAYERS / 4] |= 0x100 << layer;
        ui[DP_UI_ADDR / 4] = 0;
        ui[DP_UI_FORMAT / 4] = 1;
        ui[DP_UI_STRIDE / 4] = W * 4 | 3;
        ui[0x70 / 4] = ui[0x58 / 4] = W << 16 | H;
        ui[0x90 / 4] = 1;
        ui[0x98 / 4] = table;
        assert(scanout_layer(&s, layer, W, H, &u));
        assert(layer_fetch(&s, &u, out) == BYTES);
        for (unsigned i = 0; i < BYTES; i++)
            assert(out[i] == ((i * 7 + layer * 53) & 255));
        assert(h5p_ui_xlate(u.page_tables, 0x400000) == (hwaddr)-1);
        assert(h5p_ui_xlate(u.page_tables, 0x10000000) == (hwaddr)-1);
        put(table,
            first); /* Invalid PTE: blank, never read physical address zero. */
        fb_read(&s, 0, out, 128, u.page_tables);
        for (unsigned i = 0; i < 128; i++)
            assert(out[i] == 0);
        ui[0x90 / 4] = 0;
        assert(!scanout_layer(&s, layer, W, H, &u));
    }
    /* One read crossing the 4 MiB segment boundary must switch tables. */
    u.page_tables[0] = 0x1000;
    u.page_tables[1] = 0xc000;
    put(0x1000 + 1023 * 4, 0xe001);
    put(0xc000, 0xd001);
    memset(ram + 0xeff8, 0x35, 8);
    memset(ram + 0xd000, 0x72, 8);
    fb_read(&s, 0x3ffff8, out, 16, u.page_tables);
    for (unsigned i = 0; i < 16; i++)
        assert(out[i] == (i < 8 ? 0x35 : 0x72));
    puts(
        "PASS: H5P translates IOVA zero across scattered pages and segments, isolates layers and blanks invalid entries");
}
