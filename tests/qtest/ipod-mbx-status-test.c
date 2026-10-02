/* SPDX-License-Identifier: GPL-2.0-or-later */
/* N72 production-board STATUS/W1C, IRQ, reset and migration; no guest code. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qemu/bswap.h"

#define MBX 0x3b000000ULL
#define STATUS (MBX + 0x12c)
#define MASK (MBX + 0x130)
#define ACK (MBX + 0x134)
#define VIC_RAW 0x38e01008ULL
#define MBX_IRQ (1u << 21) /* N72 IRQ 0x35, second VIC input21 */
static char *rom, *nor, *nand;

static QTestState *start_board(bool enabled, bool incoming)
{
    return qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s,mbx-irq=%s "
                       "%s -display none -audio driver=none -nic none",
                       rom, nor, nand, enabled ? "on" : "off",
                       incoming ? "-incoming defer" : "");
}

static void irq(QTestState *q, bool high)
{
    g_assert_cmphex(qtest_readl(q, VIC_RAW) & MBX_IRQ, ==, high ? MBX_IRQ : 0);
}

static void inspect_and_ack(QTestState *q)
{
    /* Both pending bits survive observation; only enabled bit is W1C'd. */
    irq(q, true);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x548);
    irq(q, true);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x548);
    irq(q, true);
    qtest_writel(q, ACK, 0x400);
    irq(q, false);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x148);
    qtest_writel(q, MASK, 8);
    irq(q, true);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x148);
    irq(q, true);
    qtest_writel(q, ACK, 8);
    irq(q, false);
    /* Existing compatibility bits deliberately unchanged. */
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x140);
}

static void enabled(void)
{
    QTestState *q = start_board(true, false);
    qtest_writel(q, MASK, 0x400);
    qtest_writel(q, STATUS, 0x408);
    inspect_and_ack(q);
    qtest_quit(q);
}

static void off_control(void)
{
    QTestState *q = start_board(false, false);
    qtest_writel(q, MASK, 0x400);
    qtest_writel(q, STATUS, 0x408);
    inspect_and_ack(q);
    qtest_quit(q);
}

static void mask_and_reset(void)
{
    QTestState *q = start_board(true, false);
    qtest_writel(q, MASK, 0);
    qtest_writel(q, STATUS, 0x408);
    irq(q, false);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x548);
    qtest_writel(q, MASK, 0x400);
    irq(q, true);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    irq(q, false);
    g_assert_cmphex(qtest_readl(q, MASK), ==, 0);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x140);
    qtest_writel(q, MASK, 0x408);
    irq(q, false);
    qtest_quit(q);
}

static void wait_migration(QTestState *q)
{
    for (unsigned i = 0; i < 5000; i++) {
        QDict *reply = qtest_qmp(q, "{'execute':'query-migrate'}");
        const char *status = qdict_get_try_str(qdict_get_qdict(reply, "return"), "status");
        bool done = status && !strcmp(status, "completed");
        g_assert_false(status && !strcmp(status, "failed"));
        qobject_unref(reply);
        if (done) { return; }
        g_usleep(1000);
    }
    g_assert_not_reached();
}

static void restored(void)
{
    for (unsigned mode = 0; mode < 2; mode++) {
        g_autofree char *state = NULL;
        int fd = g_file_open_tmp("n72-mbx-state-XXXXXX", &state, NULL);
        g_assert_cmpint(fd, >=, 0);
        close(fd);
        g_autofree char *uri = g_strdup_printf("file:%s", state);
        QTestState *from = start_board(mode, false);
        qtest_writel(from, MASK, 0x400);
        qtest_writel(from, STATUS, 0x408);
        irq(from, true);
        qtest_qmp_assert_success(from, "{'execute':'migrate','arguments':{'uri':%s}}", uri);
        wait_migration(from);
        qtest_quit(from);
        QTestState *to = start_board(mode, true);
        /* Seed different values before load, ruling out reset defaults. */
        qtest_writel(to, MASK, 0);
        qtest_writel(to, STATUS, 1);
        irq(to, false);
        qtest_qmp_assert_success(to, "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        wait_migration(to);
        g_assert_cmphex(qtest_readl(to, MASK), ==, 0x400);
        inspect_and_ack(to);
        qtest_quit(to);
        unlink(state);
    }
}

/* Opt-in measured decoder: actual QEMU DMA to synthetic physical RAM. */
static QTestState *start_fill(bool incoming)
{
    return qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s "
                       "-global driver=ipodtouch.mbx,property=x-2d-fill,value=on %s "
                       "-display none -audio driver=none -nic none",
                       rom, nor, nand, incoming ? "-incoming defer" : "");
}

static const uint32_t fill_packet[16] = {
    0xa0060500, 0x823000, 0x94060500, 0, 0x30000000, 0x60800200,
    0x8000f0f0, 0xff000000, 0, 0x014001e0,
    0x70000000, 0x70000000, 0x70000000, 0x70000000, 0x70000000, 0x70000000
};

static void fill_memory(QTestState *q)
{
    g_assert_cmphex(qtest_readl(q, MBX + 0x1020), ==, 0);
    qtest_writel(q, MBX + 0x1020, 1);
    g_assert_cmphex(qtest_readl(q, MBX + 0x1020), ==, 0x10001);
    qtest_memset(q, 0x08001000, 0, 0x1000);
    qtest_writel(q, MBX + 0x1008, 0x08001000);
    for (unsigned i = 0; i < 150; i++) {
        qtest_writel(q, 0x08001000 + (0x23 + i) * 4, 0x08040000 + i * 8192);
        qtest_memset(q, 0x08040000 + i * 8192, 0x5a, 8192);
    }
}

static void fill_stage(QTestState *q, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        qtest_writel(q, MBX + 0xa00000 + i * 4, fill_packet[i]);
    }
}

static void fill_verify(QTestState *q)
{
    uint8_t bytes[8192];
    for (unsigned i = 0; i < 150; i++) {
        qtest_memread(q, 0x08040000 + i * 8192, bytes, sizeof bytes);
        for (unsigned j = 0; j < 4096; j += 4) {
            g_assert_cmphex((uint32_t)ldl_le_p(bytes + j), ==, 0xff000000u);
        }
        for (unsigned j = 4096; j < sizeof bytes; j++) {
            g_assert_cmphex(bytes[j], ==, 0x5a);
        }
    }
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x400);
    irq(q, true);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0x400);
    irq(q, true);
    qtest_writel(q, ACK, 0x400);
    irq(q, false);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0);
}

static void fill_execution(void)
{
    QTestState *q = start_fill(false);
    fill_memory(q);
    qtest_writel(q, MASK, 0x400);
    fill_stage(q, 16);
    irq(q, false);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0);
    g_assert_cmphex(qtest_readl(q, 0x08040000), ==, 0x5a5a5a5a);
    qtest_writel(q, MBX + 0xa00000, 0xf0000000);
    fill_verify(q);
    /* Disable uses native bypass; saved GART roots cannot authorize DMA. */
    qtest_memset(q, 0x08040000, 0x5a, 4096);
    qtest_writel(q, MBX + 0x1020, 0);
    g_assert_cmphex(qtest_readl(q, MBX + 0x1020), ==, 0);
    fill_stage(q, 16);
    qtest_writel(q, MBX + 0xa00000, 0xf0000000);
    g_assert_cmphex(qtest_readl(q, 0x08040000), ==, 0x5a5a5a5a);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0);
    irq(q, false);
    qtest_writel(q, MBX + 0x1020, 1);
    /* Old complete tail cannot qualify a newly truncated packet. */
    qtest_memset(q, 0x08040000, 0x5a, 4096);
    fill_stage(q, 15);
    qtest_writel(q, MBX + 0xa00000, 0xf0000000);
    g_assert_cmphex(qtest_readl(q, 0x08040000), ==, 0x5a5a5a5a);
    irq(q, false);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0);
    /* A late invalid PTE is refused before any output. */
    fill_stage(q, 16);
    qtest_writel(q, 0x08001000 + (0x23 + 149) * 4, 0x3b000000);
    qtest_writel(q, MBX + 0xa00000, 0xf0000000);
    g_assert_cmphex(qtest_readl(q, 0x08040000), ==, 0x5a5a5a5a);
    irq(q, false);
    qtest_writel(q, MBX + 0x838, 1);
    qtest_writel(q, MBX + 0x6d8, 0x09000000);
    qtest_writel(q, MBX + 0x814, 1);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0);
    irq(q, false);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(q, MBX + 0x1020), ==, 0);
    g_assert_cmphex(qtest_readl(q, MBX + 0x1008), ==, 0);
    g_assert_cmphex(qtest_readl(q, MBX + 0xa00000), ==, 0);
    g_assert_cmphex(qtest_readl(q, STATUS), ==, 0);
    qtest_quit(q);
}

static void fill_restored(void)
{
    g_autofree char *state = NULL;
    int fd = g_file_open_tmp("n72-mbx-fill-state-XXXXXX", &state, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    QTestState *from = start_fill(false);
    fill_memory(from);
    qtest_writel(from, MASK, 0x400);
    fill_stage(from, 16);
    qtest_qmp_assert_success(from, "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    qtest_quit(from);
    QTestState *to = start_fill(true);
    qtest_qmp_assert_success(to, "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    wait_migration(to);
    irq(to, false);
    g_assert_cmphex(qtest_readl(to, MBX + 0x1020), ==, 0x10001);
    g_assert_cmphex(qtest_readl(to, STATUS), ==, 0);
    qtest_writel(to, MBX + 0xa00000, 0xf0000000);
    fill_verify(to);
    qtest_quit(to);
    unlink(state);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd = g_file_open_tmp("n72-mbx-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n72-mbx-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("n72-mbx-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/mbx/status-observation-w1c", enabled);
    qtest_add_func("/ipod/mbx/irq-option-off-control", off_control);
    qtest_add_func("/ipod/mbx/masked-pending-reset", mask_and_reset);
    qtest_add_func("/ipod/mbx/pending-status-restored", restored);
    qtest_add_func("/ipod/mbx/measured-fill-dma-irq-refusal-reset", fill_execution);
    qtest_add_func("/ipod/mbx/measured-fill-staged-migration", fill_restored);
    int result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
