/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real N45 board: experimental ROM alias, write protection and warm reset.
 * Synthetic bytes test wiring, not stock firmware boot or hibernate. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

static char *rom, *nor, *nand;

static QTestState *start_board(void)
{
    return qtest_initf("-machine iPod-Touch-1G,bootrom=%s,nand=%s,x-rom-boot=on,wifi=off "
        "-drive if=pflash,format=raw,file=%s -display none -audio driver=none -nic none", rom, nand, nor);
}

static void pll_locks(void)
{
    QTestState *q = start_board();
    const uint64_t clock = 0x3c500000;
    /* Secondary CLOCK0 retains its existing separate behavior. */
    g_assert_cmphex(qtest_readl(q, 0x38100040), ==, 0xf);
    /* Existing direct-boot reset configuration has PLL1 and PLL3 enabled. */
    g_assert_cmphex(qtest_readl(q, clock + 0x40), ==, 0xa);
    qtest_writel(q, clock + 0x44, 0);
    g_assert_cmphex(qtest_readl(q, clock + 0x40), ==, 0);
    /* Exact native cold ROM sequence; f was the pre-fix negative baseline. */
    qtest_writel(q, clock + 0x44, 0x110);
    qtest_writel(q, clock + 0x20, 0x08005000);
    qtest_writel(q, clock + 0x30, 0x1c20);
    qtest_writel(q, clock + 0x44, 0x111);
    g_assert_cmphex(qtest_readl(q, clock + 0x40), ==, 1);
    qtest_writel(q, clock + 0x40, 0xf);
    g_assert_cmphex(qtest_readl(q, clock + 0x40), ==, 1);
    qtest_writel(q, clock + 0x20, 0x00005000);
    g_assert_cmphex(qtest_readl(q, clock + 0x40), ==, 0);
    qtest_writel(q, clock + 0x20, 0x08000000);
    g_assert_cmphex(qtest_readl(q, clock + 0x40), ==, 0);
    /* S5L8900's upper multiplier bits are valid, not N72's eight-bit field. */
    qtest_writel(q, clock + 0x20, 0x08010000);
    g_assert_cmphex(qtest_readl(q, clock + 0x40), ==, 1);
    qtest_writel(q, clock + 0x2c, 0x08004801);
    qtest_writel(q, clock + 0x44, 0x119);
    g_assert_cmphex(qtest_readl(q, clock + 0x40), ==, 9);
    qtest_writel(q, clock + 0x44, 0x118);
    g_assert_cmphex(qtest_readl(q, clock + 0x40), ==, 8);
    qtest_quit(q);
}

static void sram_aperture(void)
{
    QTestState *q = start_board();
    /* Stock N45 DT amc reg + arm-io ranges: 0x22000000, 0x2c000.
     * Verify former unmapped execution address, bank join and final word. */
    const uint64_t points[] = { 0x22000000, 0x22001000, 0x22002b98,
        0x2201fffc, 0x22020000, 0x2202bffc };
    for (unsigned i = 0; i < G_N_ELEMENTS(points); i++) {
        qtest_writel(q, points[i], 0xa5000000 | i);
        g_assert_cmphex(qtest_readl(q, points[i]), ==, 0xa5000000 | i);
    }
    /* No alias and no invented extra SRAM beyond the DT aperture. */
    qtest_writel(q, 0x21fffffc, 0x11223344);
    qtest_writel(q, 0x2202c000, 0x55667788);
    qtest_writel(q, 0x2202fffc, 0xabcdef01);
    g_assert_cmphex(qtest_readl(q, 0x21fffffc), ==, 0);
    g_assert_cmphex(qtest_readl(q, 0x2202c000), ==, 0);
    g_assert_cmphex(qtest_readl(q, 0x2202fffc), ==, 0);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    for (unsigned i = 0; i < G_N_ELEMENTS(points); i++) {
        g_assert_cmphex(qtest_readl(q, points[i]), ==, 0xa5000000 | i);
    }
    g_assert_cmphex(qtest_readl(q, 0x20000000), ==, 0xea00002f);
    qtest_quit(q);
}

static void reset_vector(void)
{
    /* No iBoot argument: this mode must not stage a direct-boot image. */
    QTestState *q = start_board();
    g_assert_cmphex(qtest_readl(q, 0), ==, 0xea00002f);
    g_assert_cmphex(qtest_readl(q, 0x20000000), ==, 0xea00002f);
    /* Supplied jump table is unchanged; no 8900 stubs in the LLB window. */
    g_assert_cmphex(qtest_readl(q, 0x2000008c), ==, 0x2000068c);
    g_assert_cmphex(qtest_readl(q, 0x22000080), ==, 0);
    g_assert_cmphex(qtest_readl(q, 0x18000000), ==, 0);
    qtest_writel(q, 0, 0);
    qtest_writel(q, 0x2000008c, 0);
    g_assert_cmphex(qtest_readl(q, 0), ==, 0xea00002f);
    g_assert_cmphex(qtest_readl(q, 0x2000008c), ==, 0x2000068c);
    qtest_writel(q, 0x08000090, 0x12345678);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(q, 0x08000090), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(q, 0), ==, 0xea00002f);
    g_assert_cmphex(qtest_readl(q, 0x2000008c), ==, 0x2000068c);
    QDict *r = qtest_qmp(q, "{'execute':'human-monitor-command','arguments':{'command-line':'info registers'}}");
    g_assert_nonnull(strstr(qdict_get_str(r, "return"), "R15=00000000"));
    qobject_unref(r);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(0x100000);
    int fd = g_file_open_tmp("n45-rom-vector-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n45-rom-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(nor, zero, 0x100000, NULL));
    const uint8_t vector[] = {0x2f, 0, 0, 0xea};
    const uint8_t jump[] = {0x8c, 6, 0, 0x20};
    memcpy(zero, vector, sizeof vector);
    memcpy(zero + 0x8c, jump, sizeof jump);
    g_assert_true(g_file_set_contents(rom, zero, 0x10000, NULL));
    nand = g_dir_make_tmp("n45-rom-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/n45/rom-reset-vector-retains-ram", reset_vector);
    qtest_add_func("/ipod/n45/rom-four-pll-lock-contract", pll_locks);
    qtest_add_func("/ipod/n45/rom-sram-aperture-retention", sram_aperture);
    int result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
