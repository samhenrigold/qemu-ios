/* SPDX-License-Identifier: GPL-2.0-or-later */
/* N72 DSIM software-reset completion on the actual board; no firmware runs. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define DSIM 0x3d800000ULL
#define SWRST_RELEASE (1U << 20)
static char *rom, *nor, *nand;

static QTestState *start_board(bool direct, bool incoming)
{
    g_autofree char *option = direct ? g_strdup_printf(",direct-iboot=%s", rom) : g_strdup("");
    return qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s%s "
                      "-display none -audio driver=none -nic none %s",
                      rom, nor, nand, option, incoming ? "-incoming defer" : "");
}

static void reset_contract(bool direct)
{
    QTestState *q = start_board(direct, false);
    g_assert_cmphex(qtest_readl(q, DSIM) & SWRST_RELEASE, ==, 0);
    qtest_writel(q, DSIM + 4, 0);
    qtest_writel(q, DSIM + 4, 2); /* Other bits cannot authorize reset completion. */
    g_assert_cmphex(qtest_readl(q, DSIM) & SWRST_RELEASE, ==, 0);
    qtest_writel(q, DSIM + 4, 1);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(q, DSIM) & SWRST_RELEASE, ==, SWRST_RELEASE);
    }
    qtest_writel(q, DSIM, 0); /* STATUS readonly; reads do not consume completion. */
    qtest_writel(q, DSIM + 4, 0);
    qtest_writel(q, DSIM + 0x2c, ~0U);
    g_assert_cmphex(qtest_readl(q, DSIM) & SWRST_RELEASE, ==, SWRST_RELEASE);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(q, DSIM) & SWRST_RELEASE, ==, 0);
    qtest_writel(q, DSIM + 4, 1);
    g_assert_cmphex(qtest_readl(q, DSIM) & SWRST_RELEASE, ==, SWRST_RELEASE);
    qtest_quit(q);
}
static void cold_reset(void) { reset_contract(false); }
static void direct_reset(void) { reset_contract(true); }

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

static void migrated_reset(bool completed)
{
    g_autofree char *state = NULL;
    int fd = g_file_open_tmp("n72-dsim-state-XXXXXX", &state, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    QTestState *from = start_board(false, false);
    if (completed) { qtest_writel(from, DSIM + 4, 1); }
    qtest_qmp_assert_success(from, "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    qtest_quit(from);
    QTestState *to = start_board(false, true);
    g_assert_cmphex(qtest_readl(to, DSIM) & SWRST_RELEASE, ==, 0);
    qtest_qmp_assert_success(to, "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    wait_migration(to);
    for (unsigned i = 0; i < 4; i++) {
        g_assert_cmphex(qtest_readl(to, DSIM) & SWRST_RELEASE, ==,
                       completed ? SWRST_RELEASE : 0);
    }
    qtest_qmp_assert_success(to, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(to, DSIM) & SWRST_RELEASE, ==, 0);
    qtest_quit(to);
    unlink(state);
}
static void migrated_pending(void) { migrated_reset(false); }
static void migrated_completed(void) { migrated_reset(true); }

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;
    fd = g_file_open_tmp("n72-dsim-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n72-dsim-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("n72-dsim-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/dsim/stock-cold-software-reset", cold_reset);
    qtest_add_func("/ipod/dsim/direct-software-reset", direct_reset);
    qtest_add_func("/ipod/dsim/migrated-before-software-reset", migrated_pending);
    qtest_add_func("/ipod/dsim/migrated-after-software-reset", migrated_completed);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
