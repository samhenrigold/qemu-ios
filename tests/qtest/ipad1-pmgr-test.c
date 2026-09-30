/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise the production PMGR on its board MMIO/VIC wiring. No guest firmware
 * is executed: the zero-filled ROM only satisfies the board's image-size
 * requirement, and libqtest supplies the accelerator and virtual clock. */
#include "qemu/osdep.h"
#include "libqtest.h"

#define PMGR 0xbf100000ULL
#define VIC0 0xbf200000ULL
#define RAW_IRQ 0x08
#define TIMER0_IRQ (1U << 6)
#define TIMER1_IRQ (1U << 5)
#define COUNT(n) (PMGR + 0x2008 + 4 * (n))
#define STATE(n) (PMGR + 0x2010 + 4 * (n))
#define START 1
#define UPDATE 2

static char *rom_path;

static QTestState *start_board(void)
{
    return qtest_initf("-machine ipad1,bootrom=%s -display none "
                       "-audio driver=none -nic none", rom_path);
}

static void test_timebase(void)
{
    QTestState *qts = start_board();
    uint32_t before = qtest_readl(qts, PMGR + 0x2000);

    qtest_clock_step(qts, 1000000);
    g_assert_cmpuint(qtest_readl(qts, PMGR + 0x2000) - before, ==, 24000);
    g_assert_cmpuint(qtest_readl(qts, PMGR + 0x2004), ==, 0);
    /* Gate requests mirror into actual bits; reset pulses self-clear. */
    qtest_writel(qts, PMGR + 0x1010, 0x80000003);
    g_assert_cmphex(qtest_readl(qts, PMGR + 0x1010), ==, 0x33);
    qtest_writel(qts, PMGR + 0x1010, 0);
    g_assert_cmphex(qtest_readl(qts, PMGR + 0x1010), ==, 0);
    qtest_quit(qts);
}

static void test_event_irq(void)
{
    QTestState *qts = start_board();
    uint32_t timers = TIMER0_IRQ | TIMER1_IRQ;

    g_assert_cmphex(qtest_readl(qts, VIC0 + RAW_IRQ) & timers, ==, 0);
    qtest_writel(qts, COUNT(0), 24000);
    qtest_writel(qts, COUNT(1), 48000);
    qtest_writel(qts, STATE(0), START | UPDATE);
    qtest_writel(qts, STATE(1), START | UPDATE);
    qtest_clock_step(qts, 999999);
    g_assert_cmphex(qtest_readl(qts, VIC0 + RAW_IRQ) & timers, ==, 0);
    g_assert_cmpuint(qtest_readl(qts, COUNT(0)), ==, 1);
    qtest_clock_step(qts, 2);
    g_assert_cmphex(qtest_readl(qts, VIC0 + RAW_IRQ) & timers, ==, TIMER0_IRQ);
    g_assert_cmpuint(qtest_readl(qts, COUNT(0)), ==, 0);
    /* UPDATE acknowledges the level; no START means no new expiry. */
    qtest_writel(qts, STATE(0), UPDATE);
    g_assert_cmphex(qtest_readl(qts, VIC0 + RAW_IRQ) & timers, ==, 0);
    qtest_clock_step(qts, 1000000);
    g_assert_cmphex(qtest_readl(qts, VIC0 + RAW_IRQ) & timers, ==, TIMER1_IRQ);
    qtest_writel(qts, STATE(1), UPDATE);
    g_assert_cmphex(qtest_readl(qts, VIC0 + RAW_IRQ) & timers, ==, 0);
    /* A disabled timer cannot later assert its stale scheduled interrupt. */
    qtest_writel(qts, COUNT(0), 24000);
    qtest_writel(qts, STATE(0), START | UPDATE);
    qtest_writel(qts, STATE(0), 0);
    qtest_clock_step(qts, 2000000);
    g_assert_cmphex(qtest_readl(qts, VIC0 + RAW_IRQ) & timers, ==, 0);
    qtest_quit(qts);
}

static void test_watchdog_reset(void)
{
    QTestState *qts = start_board();

    qtest_writel(qts, PMGR + 0x2020, 0);
    qtest_writel(qts, PMGR + 0x2024, 24000);
    qtest_writel(qts, PMGR + 0x202c, 4);
    qtest_clock_step(qts, 500000);
    g_assert_cmpuint(qtest_readl(qts, PMGR + 0x2020), ==, 12000);
    /* Feeding moves the deadline; it must not expire at the original time. */
    qtest_writel(qts, PMGR + 0x2020, 0);
    qtest_clock_step(qts, 500001);
    g_assert_cmphex(qtest_readl(qts, PMGR + 0x202c), ==, 4);
    /* Expiry requests a board reset without another register access. */
    qtest_clock_step(qts, 500000);
    qtest_qmp_eventwait(qts, "RESET");
    g_assert_cmphex(qtest_readl(qts, PMGR + 0x202c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, PMGR + 0x2020), ==, 0);
    qtest_quit(qts);
}

int main(int argc, char **argv)
{
    g_autofree char *rom = g_malloc0(65536);
    int fd = g_file_open_tmp("ipad1-qtest-rom-XXXXXX", &rom_path, NULL);
    int result;

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(rom_path, rom, 65536, NULL));
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipad1/pmgr/timebase-gates", test_timebase);
    qtest_add_func("/ipad1/pmgr/event-vic-irq", test_event_irq);
    qtest_add_func("/ipad1/pmgr/watchdog-reset", test_watchdog_reset);
    result = g_test_run();
    unlink(rom_path);
    g_free(rom_path);
    return result;
}
