/* The A4 CLCD's output gamma and frame interrupt: White on Black on the iPhone 4, iPod touch 4G and iPad.
 *
 * AppleCLCD reads the R, G and B tables back (+0x34 = table << 12 | entry, data at +0x3c), mirrors them (8C148
 * 0x809da7e0), turns its frame interrupt on (+0x68 = 0xf, +0x64 = 4, +0x08 = 1) and loads the mirror from that
 * interrupt (0x809dbbac): +0x34 = 0x14000 and 256 writes to +0x38 per table. The panel shows every pixel through the
 * tables while +0x2c bit0 is on, as iBoot leaves it, as a panel calibrated for the table it was given shows them.
 *
 * Mutations (each fails this test): the interrupt line ignoring the status (the load never runs), +0x3c reading 0
 * (the mirror is all black), the entry not stepping after a +0x38 write, the tables not applied in gamma_row, the
 * tables applied with +0x2c bit0 off (7.x's all-zero load blacks the panel), the loaded table shown as it is (the
 * iPad's calibration curve darkens the screen).
 *
 * SLICE include/hw/arm/panel-gamma.h file
 * SLICE hw/arm/s5l8930_display.c define (CLCD_|CLCD_WORDS)
 * SLICE hw/arm/s5l8930_display.c fn clcd_update_irq clcd_frame_end gamma_update gamma_reset clcd_read clcd_write gamma_row
 */
#include <assert.h>
#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t hwaddr;
#define S5L8930_CLCD_SIZE 0x2000
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
typedef int DisplayPipe;
typedef int *qemu_irq;
static void qemu_set_irq(qemu_irq irq, int level) { *irq = level; }
static uint32_t panel_word(const DisplayPipe *p, hwaddr addr, uint32_t v) { (void)p; (void)addr; return v; }
#define CLCD_WORDS_ (0x2000 / 4)
typedef struct S5L8930DisplayState {
    DisplayPipe pipe[1];
    qemu_irq clcd_irq;
    uint32_t clcd[2][CLCD_WORDS_];
    uint32_t lut[8][256];
    uint32_t clcd_status;
    uint32_t calib[3][256];
    uint8_t gamma8[3][256];
    bool gamma_identity;
    bool h5p;
    int shown;
} S5L8930DisplayState;
#include "slice.h"

/* AppleCLCD's mirror (0x809da7e0): entry i becomes entry 255 - i with its two 10-bit halves swapped. */
static void mirror(uint32_t t[256])
{
    uint32_t c[256];

    memcpy(c, t, sizeof(c));
    for (int i = 0; i < 256; i++) {
        uint32_t e = c[255 - i];
        t[i] = (e & 0x3ff) << 10 | ((e >> 10) & 0x3ff);
    }
}

static uint32_t px(const S5L8930DisplayState *s, uint32_t p)
{
    uint32_t o;

    gamma_row(s, &p, &o, 1);
    return o;
}

int main(void)
{
    static S5L8930DisplayState st;
    S5L8930DisplayState *s = &st;
    int irq = 0;
    uint32_t tab[3][256];

    s->clcd_irq = &irq;
    gamma_reset(s);
    assert(s->gamma_identity && px(s, 0xffa1b2c3) == 0xffa1b2c3);

    /* White on Black, as AppleCLCD does it: read, mirror, interrupt on. */
    for (int t = 0; t < 3; t++) {
        for (int i = 0; i < 256; i++) {
            clcd_write(s, CLCD_LUT_CTRL, (4 + t) << 12 | i, 4);
            tab[t][i] = clcd_read(s, CLCD_LUT_RDATA, 4);
        }
        mirror(tab[t]);
    }
    clcd_write(s, CLCD_IRQ_STATUS, 0xf, 4);
    clcd_write(s, CLCD_IRQ_MASK, 4, 4);
    clcd_write(s, CLCD_IRQ_ENABLE, 1, 4);
    assert(!irq);
    clcd_frame_end(s);   /* the next frame (vbl_tick) */
    assert(irq);
    /* its filter: acknowledge, then load each table from one control write */
    clcd_write(s, CLCD_IRQ_STATUS, 0xf, 4);
    assert(!irq);
    for (int t = 0; t < 3; t++) {
        clcd_write(s, CLCD_LUT_CTRL, 0x14000 + t * 0x1000, 4);
        for (int i = 0; i < 256; i++) {
            clcd_write(s, CLCD_LUT_WDATA, 0x80000000u | tab[t][i], 4);
        }
    }
    assert(!s->gamma_identity);
    /* inverted: 0xa1 -> 0xfe - 0xa1 */
    assert(px(s, 0xffa1b2c3) == (0xff000000u | (0xfe - 0xa1) << 16 | (0xfe - 0xb2) << 8 | (0xfe - 0xc3)));
    assert(px(s, 0xff000000) == 0xfffefefe && px(s, 0xffffffff) == 0xff000000);
    /* reading back what was loaded */
    clcd_write(s, CLCD_LUT_CTRL, 5 << 12 | 7, 4);
    assert(clcd_read(s, CLCD_LUT_RDATA, 4) == tab[1][7]);
    /* +0x2c bit0 off: the panel bypasses the tables (4.x clears it only around a load) */
    clcd_write(s, CLCD_GAMMA_CTRL, 0, 4);
    assert(s->gamma_identity && px(s, 0xffa1b2c3) == 0xffa1b2c3);
    clcd_write(s, CLCD_GAMMA_CTRL, 1, 4);
    assert(px(s, 0xff000000) == 0xfffefefe);
    /* 7.x with the gamma off (11D257): it loads a table it never read, all zero, and leaves the bit off */
    clcd_write(s, CLCD_GAMMA_CTRL, 0, 4);
    for (int t = 0; t < 3; t++) {
        clcd_write(s, CLCD_LUT_CTRL, 0x14000 + t * 0x1000, 4);
        for (int i = 0; i < 256; i++) {
            clcd_write(s, CLCD_LUT_WDATA, 0x80000000u, 4);
        }
    }
    clcd_write(s, CLCD_GAMMA_CTRL, clcd_read(s, CLCD_GAMMA_CTRL, 4) & ~9u, 4);
    assert(px(s, 0xffa1b2c3) == 0xffa1b2c3);
    /* The iPad's 5.x loads its panel's calibration at boot, a curve: the panel shows the framebuffer as drawn, and
     * that curve mirrored inverts it. */
    clcd_write(s, CLCD_GAMMA_CTRL, 0, 4);
    for (int t = 0; t < 3; t++) {
        clcd_write(s, CLCD_LUT_CTRL, 0x14000 + t * 0x1000, 4);
        for (int i = 0; i < 256; i++) {
            unsigned prev = lround(1023 * pow(MAX(i - 1, 0) / 255.0, 2.2)), cur = lround(1023 * pow(i / 255.0, 2.2));
            tab[t][i] = prev << 10 | cur;
            clcd_write(s, CLCD_LUT_WDATA, 0x80000000u | tab[t][i], 4);
        }
    }
    clcd_write(s, CLCD_GAMMA_CTRL, 1, 4);
    assert(s->gamma_identity && px(s, 0xffa1b2c3) == 0xffa1b2c3);
    for (int t = 0; t < 3; t++) {
        mirror(tab[t]);
        clcd_write(s, CLCD_LUT_CTRL, 0x14000 + t * 0x1000, 4);
        for (int i = 0; i < 256; i++) {
            clcd_write(s, CLCD_LUT_WDATA, 0x80000000u | tab[t][i], 4);
        }
    }
    for (unsigned v = 40; v < 230; v += 13) {   /* in 10 bits a 2.2 curve is flat near black: left out */
        unsigned o = px(s, 0xff000000u | v << 16) >> 16 & 0xff;
        assert(o + 2 >= 254 - v && o <= 254 - v + 2);
    }
    /* H5P's second gamma RPC polls the first update's request bit. Preserve
     * enable/control bits and retain the older variant's register behavior. */
    clcd_write(s, CLCD_GAMMA_CTRL, 0x49, 4);
    clcd_frame_end(s);
    assert(clcd_read(s, CLCD_GAMMA_CTRL, 4) == 0x49);
    s->h5p = true;
    clcd_write(s, CLCD_GAMMA_CTRL, 0x49, 4);
    assert(clcd_read(s, CLCD_GAMMA_CTRL, 4) & CLCD_GAMMA_UPDATE);
    clcd_frame_end(s);
    assert(clcd_read(s, CLCD_GAMMA_CTRL, 4) == 9);
    clcd_write(s, CLCD_GAMMA_CTRL, 0x48, 4);
    clcd_frame_end(s);
    assert(clcd_read(s, CLCD_GAMMA_CTRL, 4) == 8);
    puts("PASS: H5P gamma updates complete; the frame interrupt loads White on Black's mirrored tables and the panel shows them inverted; "
         "with +0x2c bit0 off the panel bypasses them; a calibration curve shows the framebuffer as drawn");
    return 0;
}
