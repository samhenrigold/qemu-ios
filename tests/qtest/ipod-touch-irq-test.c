/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual N45 touch -> SYSIC -> VIC wiring; no firmware executes. */
#include "qemu/osdep.h"
#include "libqtest.h"
#define SYSIC 0x39a00000ULL
#define VIC 0x38e00000ULL
#define TOUCH (1U << 27)
#define GROUP_IRQ (1U << 2)
static char *rom, *nor, *nand;

static void touch_mask(void)
{
    QTestState *q = qtest_initf("-machine iPod-Touch-1G,bootrom=%s,iboot=%s,nand=%s,wifi=off "
                              "-drive if=pflash,format=raw,file=%s -display none -audio driver=none",
                              rom, rom, nand, nor);
    qtest_qmp_assert_success(q, "{ 'execute': 'input-send-event', 'arguments': { 'events': ["
                            "{ 'type': 'btn', 'data': { 'button': 'left', 'down': true } }] } }");
    /* Masking gates the shared output, not the edge latch. Enabling an already
     * pending edge presents it; ACK clears it; the next report re-latches it. */
    g_assert_cmphex(qtest_readl(q, SYSIC + 0xb0) & TOUCH, ==, TOUCH);
    g_assert_cmphex(qtest_readl(q, VIC + 8) & GROUP_IRQ, ==, 0);
    qtest_writel(q, SYSIC + 0xd0, TOUCH);
    g_assert_cmphex(qtest_readl(q, VIC + 8) & GROUP_IRQ, ==, GROUP_IRQ);
    qtest_writel(q, SYSIC + 0xd0, 0);
    g_assert_cmphex(qtest_readl(q, VIC + 8) & GROUP_IRQ, ==, 0);
    qtest_writel(q, SYSIC + 0xd0, TOUCH);
    qtest_writel(q, SYSIC + 0xb0, TOUCH);
    g_assert_cmphex(qtest_readl(q, VIC + 8) & GROUP_IRQ, ==, 0);
    qtest_clock_step(q, 20000000);
    g_assert_cmphex(qtest_readl(q, VIC + 8) & GROUP_IRQ, ==, GROUP_IRQ);
    qtest_quit(q);
}
int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;
    fd = g_file_open_tmp("touch-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("touch-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 65536, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("touch-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/touch/masked-atn", touch_mask);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
