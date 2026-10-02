/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual N45 touch/PMU -> SYSIC -> VIC wiring; no firmware executes. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
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

static void sequence_status(QTestState *q, int id, const char *expected)
{
    QDict *reply = qtest_qmp(q, "{ 'execute': 'query-input-sequence', "
                              "'arguments': { 'id': %d } }", id);
    g_assert_false(qdict_haskey(reply, "error"));
    g_assert_cmpstr(qdict_get_str(qdict_get_qdict(reply, "return"), "status"),
                    ==, expected);
    qobject_unref(reply);
}

static void send_power_sequence(QTestState *q, int id)
{
    qtest_qmp_assert_success(q, "{ 'execute': 'input-send-sequence', 'arguments': {"
        "'id': %d, 'events': ["
        "{'type':'key','at-ms':0,'key':'meta_l','down':true},"
        "{'type':'key','at-ms':10,'key':'l','down':true},"
        "{'type':'key','at-ms':30,'key':'l','down':false},"
        "{'type':'key','at-ms':30,'key':'meta_l','down':false}]}}", id);
}

/* Exercise generated QAPI, actual handler ownership and virtual deadlines,
 * and observe hardware EXTON1 edges rather than only an internal counter. */
static void input_sequence(void)
{
    QTestState *q = start_board();
    pmu_write(q, 8, 0xf3); /* unmask EXTON1 rise and fall */
    send_power_sequence(q, 1);
    sequence_status(q, 1, "running");
    qtest_clock_step(q, 9000000);
    g_assert_cmphex(pmu_read(q, 3), ==, 0);
    qtest_clock_step(q, 1000000);
    g_assert_cmphex(pmu_read(q, 3), ==, 4);
    /* A malformed replacement may not steal the currently held power key. */
    QDict *reply = qtest_qmp(q, "{'execute':'input-send-sequence','arguments':"
        "{'id':2,'events':[{'type':'key','at-ms':0,'key':'l','down':true}]}}");
    g_assert_true(qdict_haskey(reply, "error"));
    qobject_unref(reply);
    sequence_status(q, 1, "running");
    qtest_clock_step(q, 20000000);
    g_assert_cmphex(pmu_read(q, 3), ==, 8);
    sequence_status(q, 1, "completed");
    sequence_status(q, 2, "unknown");

    send_power_sequence(q, 3);
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(pmu_read(q, 3), ==, 4);
    qtest_qmp_assert_success(q, "{'execute':'stop'}");
    qtest_qmp_assert_success(q, "{'execute':'input-cancel-sequence',"
                               "'arguments':{'id':99}}");
    sequence_status(q, 3, "running");
    qtest_qmp_assert_success(q, "{'execute':'input-cancel-sequence',"
                               "'arguments':{'id':3}}");
    sequence_status(q, 3, "cancelled");
    g_assert_cmphex(pmu_read(q, 3), ==, 8); /* owned release reaches paused board */
    qtest_qmp_assert_success(q, "{'execute':'cont'}");
    send_power_sequence(q, 4);
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(pmu_read(q, 3), ==, 4);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    sequence_status(q, 4, "cancelled");
    qtest_clock_step(q, 30000000);
    sequence_status(q, 4, "cancelled");
    /* Admission reads the real manually held handler state, not a test shim. */
    qtest_qmp_assert_success(q, "{'execute':'input-send-event','arguments':"
        "{'events':[{'type':'btn','data':{'button':'left','down':true}}]}}");
    reply = qtest_qmp(q, "{'execute':'input-send-sequence','arguments':"
        "{'id':5,'events':[{'type':'touch','at-ms':10,'phase':'begin','x':0.2,'y':0.3},"
        "{'type':'touch','at-ms':30,'phase':'end','x':0.2,'y':0.3}]}}");
    g_assert_true(qdict_haskey(reply, "error"));
    qobject_unref(reply);
    qtest_qmp_assert_success(q, "{'execute':'input-send-event','arguments':"
        "{'events':[{'type':'btn','data':{'button':'left','down':false}}]}}");
    qtest_writel(q, SYSIC + 0xb0, TOUCH);
    qtest_qmp_assert_success(q, "{'execute':'input-send-sequence','arguments':"
        "{'id':5,'events':[{'type':'touch','at-ms':10,'phase':'begin','x':0.2,'y':0.3},"
        "{'type':'touch','at-ms':30,'phase':'end','x':0.2,'y':0.3}]}}");
    qtest_clock_step(q, 10000000);
    g_assert_cmphex(qtest_readl(q, SYSIC + 0xb0) & TOUCH, ==, TOUCH);
    sequence_status(q, 5, "running");
    qtest_clock_step(q, 20000000);
    sequence_status(q, 5, "completed");
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
    qtest_add_func("/ipod/input/virtual-sequence", input_sequence);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
