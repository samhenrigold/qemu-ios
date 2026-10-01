/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual N45 touch/PMU -> SYSIC -> VIC wiring; no firmware executes. */
#include "qemu/osdep.h"
#include "libqtest.h"
#define SYSIC 0x39a00000ULL
#define VIC 0x38e00000ULL
#define TOUCH (1U << 27)
#define GROUP_IRQ (1U << 2)
static char *rom, *nor, *nand;

static QTestState *start_board(void)
{
    return qtest_initf("-machine iPod-Touch-1G,bootrom=%s,iboot=%s,nand=%s,wifi=off "
                              "-drive if=pflash,format=raw,file=%s -display none -audio driver=none",
                              rom, rom, nand, nor);
}

static void touch_mask(void)
{
    QTestState *q = start_board();
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
#define I2C 0x3c900000ULL
#define PMU_PIN (1U << 21) /* GPIO interrupt 0x55: group 2, bit 21 */
#define PMU_VIC (1U << 31)

static void pmu_select(QTestState *q, unsigned reg)
{
    qtest_writel(q, I2C + 4, 0xd0); /* stop / master transmit */
    qtest_writel(q, I2C + 0xc, 0xe6); /* PMU 0x73 + write */
    qtest_writel(q, I2C + 4, 0xf0);
    qtest_writel(q, I2C + 0xc, reg);
}

static void pmu_write(QTestState *q, unsigned reg, unsigned value)
{
    pmu_select(q, reg);
    qtest_writel(q, I2C + 0xc, value);
    qtest_writel(q, I2C + 4, 0xd0);
}

static unsigned pmu_read(QTestState *q, unsigned reg)
{
    pmu_select(q, reg);
    qtest_writel(q, I2C + 4, 0xd0);
    qtest_writel(q, I2C + 4, 0x90); /* master receive */
    qtest_writel(q, I2C + 0xc, 0xe7);
    qtest_writel(q, I2C + 4, 0xb0);
    unsigned value = qtest_readl(q, I2C + 0xc);
    qtest_writel(q, I2C + 4, 0x90);
    return value;
}

static void power(QTestState *q, bool down)
{
    qtest_qmp_assert_success(q, "{ 'execute': 'input-send-event', 'arguments': { 'events': ["
                            "{ 'type': 'key', 'data': { 'key': { 'type': 'qcode', 'data': 'meta_l' }, 'down': true } },"
                            "{ 'type': 'key', 'data': { 'key': { 'type': 'qcode', 'data': 'l' }, 'down': %i } }] } }",
                            down);
}

static void pmu_wake_irq(void)
{
    QTestState *q = start_board();
    for (unsigned i = 0; i < 5; i++) {
        g_assert_cmphex(pmu_read(q, 2 + i), ==, 0);
        g_assert_cmphex(pmu_read(q, 7 + i), ==, 0xff);
    }
    qtest_writel(q, SYSIC + 0xc8, PMU_PIN);
    power(q, true);
    /* Masked EXTON1 rise is latched, then appears on enabling INT2M. */
    g_assert_cmphex(qtest_readl(q, SYSIC + 0xa8) & PMU_PIN, ==, 0);
    pmu_write(q, 8, 0xfb);
    g_assert_cmphex(qtest_readl(q, SYSIC + 0xa8) & PMU_PIN, ==, PMU_PIN);
    g_assert_cmphex(qtest_readl(q, VIC + 8) & PMU_VIC, ==, PMU_VIC);
    /* ACKing the parent cannot consume a still-asserted PMU level. */
    qtest_writel(q, SYSIC + 0xa8, PMU_PIN);
    g_assert_cmphex(qtest_readl(q, SYSIC + 0xa8) & PMU_PIN, ==, PMU_PIN);
    g_assert_cmphex(pmu_read(q, 3), ==, 4);
    g_assert_cmphex(qtest_readl(q, SYSIC + 0xa8) & PMU_PIN, ==, 0);
    g_assert_cmphex(qtest_readl(q, VIC + 8) & PMU_VIC, ==, 0);
    g_assert_cmphex(pmu_read(q, 3), ==, 0);
    power(q, false);
    pmu_write(q, 8, 0xf7);
    g_assert_cmphex(qtest_readl(q, SYSIC + 0xa8) & PMU_PIN, ==, PMU_PIN);
    g_assert_cmphex(pmu_read(q, 2), ==, 0); /* INT1 does not hold EXTON1 */
    g_assert_cmphex(pmu_read(q, 3), ==, 8);
    g_assert_cmphex(qtest_readl(q, SYSIC + 0xa8) & PMU_PIN, ==, 0);
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
    qtest_add_func("/ipod/pmu/exton1-wake-irq", pmu_wake_irq);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
