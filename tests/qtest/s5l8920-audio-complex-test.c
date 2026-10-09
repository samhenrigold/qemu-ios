/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The S5L8920X audio complex's NCOs (hw/arm/s5l8920.c): AppleS5L8920XAudioComplex
 * sets a port's clock f by writing +0x18 = 2 f and +0x1c = 2 f - ncoref, and reads
 * the two back to report MCLK = ncoref * [+0x18] / ([+0x18] - [+0x1c]) / 2 (7E18
 * 0xc067832c, 0xc06784f4). Reading 0 made every port 0 Hz and left 3.1.3 silent.
 */
#include "qemu/osdep.h"
#include "libqtest.h"

#define NCO_CTRL 0x84300014
#define NCO_A    0x84300018
#define NCO_B    0x8430001c
#define NCOREF   162000000u     /* the ncoref-frequency FirmwareKit's KBoot fills: PLL1 */

static char *kboot;

static void check(const char *machine)
{
    QTestState *q = qtest_initf("-machine %s,kboot=%s,wifi=off -display none -audio driver=none -nic none",
                                machine, kboot);
    uint32_t f = 64 * 44100, a, b;

    qtest_writel(q, NCO_A, 2 * f);
    qtest_writel(q, NCO_B, 2 * f - NCOREF);
    qtest_writel(q, NCO_CTRL, 0xd00);
    a = qtest_readl(q, NCO_A);
    b = qtest_readl(q, NCO_B);
    g_assert_cmpuint(a - b, ==, NCOREF);
    g_assert_cmpuint((uint64_t)NCOREF * a / (a - b) / 2, ==, f);
    qtest_quit(q);
}

static void n88(void) { check("n88"); }
static void n18(void) { check("n18"); }

int main(int argc, char **argv)
{
    /* A K48KBOOT bundle with an empty image: the trailer only. */
    static const uint8_t trailer[24] = {
        'K', '4', '8', 'K', 'B', 'O', 'O', 'T',
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40,
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
    };
    int fd, ret;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("s5l8920-kboot-XXXXXX", &kboot, NULL);
    g_assert(fd >= 0);
    g_assert(write(fd, trailer, sizeof(trailer)) == sizeof(trailer));
    close(fd);
    qtest_add_func("/s5l8920/audio-complex/n88-nco", n88);
    qtest_add_func("/s5l8920/audio-complex/n18-nco", n18);
    ret = g_test_run();
    unlink(kboot);
    g_free(kboot);
    return ret;
}
