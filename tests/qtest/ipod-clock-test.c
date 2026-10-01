/* SPDX-License-Identifier: GPL-2.0-or-later */
/* N72 root clock on the production board; no guest instructions execute. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qlist.h"
#include "qobject/qnum.h"

#define ROOT 0x3c500000ULL
#define CONFIG0 (ROOT + 0)
#define CONFIG1 (ROOT + 4)
#define PLL0 (ROOT + 0x20)
#define LOCK (ROOT + 0x40)
#define MODE (ROOT + 0x44)
#define PERIOD_1SEC (1000000000ULL << 32)
static char *rom, *nor, *nand;

static QTestState *start_board(void)
{
    return qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s "
                       "-display none -audio driver=none -nic none", rom, nor, nand);
}

static uint64_t pclk_period(QTestState *q)
{
    QDict *response = qtest_qmp(q, "{'execute':'qom-list',"
                                 "'arguments':{'path':'/machine/unattached'}}");
    QList *children = qdict_get_qlist(response, "return");
    const QListEntry *entry;
    uint64_t period = 0;
    bool found = false;

    QLIST_FOREACH_ENTRY(children, entry) {
        QDict *child = qobject_to(QDict, qlist_entry_obj(entry));
        if (!strcmp(qdict_get_str(child, "type"), "child<ipodtouch.clock>")) {
            g_autofree char *path = g_strdup_printf("/machine/unattached/%s",
                                                   qdict_get_str(child, "name"));
            QDict *reply = qtest_qmp(q, "{'execute':'qom-get', 'arguments':"
                                      "{'path':%s,'property':'s5l8720'}}", path);
            bool root = qdict_get_bool(reply, "return");
            qobject_unref(reply);
            if (root) {
                g_autofree char *output = g_strdup_printf("%s/pclk", path);
                reply = qtest_qmp(q, "{'execute':'qom-get', 'arguments':"
                                    "{'path':%s,'property':'qtest-clock-period'}}", output);
                period = qnum_get_uint(qobject_to(QNum, qdict_get(reply, "return")));
                qobject_unref(reply);
                found = true;
                break;
            }
        }
    }
    qobject_unref(response);
    g_assert_true(found);
    return period;
}

static void locks_and_readback(void)
{
    QTestState *q = start_board();
    g_assert_cmphex(qtest_readl(q, LOCK), ==, 0);
    /* Enable alone cannot lock an unconfigured PLL; each PLL is independent. */
    qtest_writel(q, MODE, 7);
    g_assert_cmphex(qtest_readl(q, LOCK), ==, 0);
    qtest_writel(q, PLL0, 0x03008500);
    g_assert_cmphex(qtest_readl(q, LOCK), ==, 1);
    qtest_writel(q, ROOT + 0x24, 0x06005101);
    g_assert_cmphex(qtest_readl(q, LOCK), ==, 3);
    qtest_writel(q, ROOT + 0x28, 0x02004002);
    g_assert_cmphex(qtest_readl(q, LOCK), ==, 7);
    qtest_writel(q, MODE, 6);
    g_assert_cmphex(qtest_readl(q, LOCK), ==, 6);
    /* A zero input divider must not be treated as a valid locked PLL. */
    qtest_writel(q, ROOT + 0x24, 0x00005101);
    g_assert_cmphex(qtest_readl(q, LOCK), ==, 4);
    qtest_writel(q, ROOT + 0x30, 0xe10);
    g_assert_cmphex(qtest_readl(q, ROOT + 0x30), ==, 0xe10);
    qtest_writel(q, LOCK, 7);
    g_assert_cmphex(qtest_readl(q, LOCK), ==, 4);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(q, LOCK), ==, 0);
    g_assert_cmphex(qtest_readl(q, ROOT + 0x30), ==, 0);
    qtest_quit(q);
}

static void peripheral_clock(void)
{
    QTestState *q = start_board();
    g_assert_cmpuint(pclk_period(q), ==, 0);
    /* Stock ROM/LLB sequence: 24 MHz * 133 / 3, peripheral divisor 4. */
    qtest_writel(q, PLL0, 0x03008500);
    qtest_writel(q, MODE, 0x00010001);
    qtest_writel(q, CONFIG1, 0x00424242);
    qtest_writel(q, CONFIG0, 0x100a);
    g_assert_cmpuint(pclk_period(q), ==, PERIOD_1SEC / 266000000);
    /* Main PLL is sampled before SDIV, unlike the auxiliary PLL outputs. */
    qtest_writel(q, PLL0, 0x03008507);
    g_assert_cmpuint(pclk_period(q), ==, PERIOD_1SEC / 266000000);
    qtest_writel(q, CONFIG1, 0);
    g_assert_cmpuint(pclk_period(q), ==, PERIOD_1SEC / 1064000000);
    qtest_writel(q, CONFIG1, 0x4200); /* peripheral divisor four again */
    qtest_writel(q, MODE, 0x11); /* PLL0's 27 MHz reference */
    g_assert_cmpuint(pclk_period(q), ==, PERIOD_1SEC / 299250000);
    /* Select PLL1 and its reference independently, then disable it. */
    qtest_writel(q, ROOT + 0x24, 0x06005101);
    qtest_writel(q, MODE, 3);
    qtest_writel(q, CONFIG0, 0x2000);
    g_assert_cmpuint(pclk_period(q), ==, PERIOD_1SEC / 81000000);
    qtest_writel(q, MODE, 1);
    g_assert_cmpuint(pclk_period(q), ==, 0);
    qtest_writel(q, MODE, 3);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmpuint(pclk_period(q), ==, 0);
    qtest_quit(q);
}

static void wait_migration(QTestState *q)
{
    for (unsigned i = 0; i < 5000; i++) {
        QDict *reply = qtest_qmp(q, "{'execute':'query-migrate'}");
        const char *status = qdict_get_try_str(qdict_get_qdict(reply, "return"), "status");
        bool complete = status && !strcmp(status, "completed");
        g_assert_false(status && !strcmp(status, "failed"));
        qobject_unref(reply);
        if (complete) {
            return;
        }
        g_usleep(1000);
    }
    g_assert_not_reached();
}

static void restored_clock(void)
{
    g_autofree char *state = NULL;
    g_autofree char *uri = NULL;
    int fd = g_file_open_tmp("n72-clock-state-XXXXXX", &state, NULL);
    QTestState *from = start_board();
    QTestState *to;

    g_assert_cmpint(fd, >=, 0); close(fd);
    uri = g_strdup_printf("file:%s", state);
    qtest_writel(from, PLL0, 0x03008500);
    qtest_writel(from, MODE, 0x10001);
    qtest_writel(from, CONFIG1, 0x424242);
    qtest_writel(from, CONFIG0, 0x100a);
    qtest_qmp_assert_success(from, "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    qtest_quit(from);
    to = qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s "
                     "-incoming defer -display none -audio driver=none -nic none", rom, nor, nand);
    g_assert_cmpuint(pclk_period(to), ==, 0);
    qtest_qmp_assert_success(to, "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    wait_migration(to);
    /* The output is derived after loading the actual registers, not serialized
     * as a separate authority that could disagree with them. */
    g_assert_cmphex(qtest_readl(to, CONFIG1), ==, 0x424242);
    g_assert_cmphex(qtest_readl(to, LOCK), ==, 1);
    g_assert_cmpuint(pclk_period(to), ==, PERIOD_1SEC / 266000000);
    qtest_writel(to, MODE, 0);
    g_assert_cmpuint(pclk_period(to), ==, 0);
    qtest_quit(to);
    unlink(state);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;
    fd = g_file_open_tmp("n72-clock-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n72-clock-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("n72-clock-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/clock/pll-locks-readback-reset", locks_and_readback);
    qtest_add_func("/ipod/clock/peripheral-frequency", peripheral_clock);
    qtest_add_func("/ipod/clock/restored-frequency", restored_clock);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
