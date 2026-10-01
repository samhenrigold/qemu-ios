/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real FMSS state, no executing firmware or generated NAND fixture. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define FMSS 0x38a00000ULL
#define RAM 0x08010000ULL
static char *rom_path, *nor_path, *nand_path;

static QTestState *start_board(char **overlay)
{
    *overlay = g_dir_make_tmp("fmss-overlay-XXXXXX", NULL);
    return qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s,nandrw=%s "
                       "-display none -audio driver=none -nic none",
                       rom_path, nor_path, nand_path, *overlay);
}

static bool migrate(QTestState *qts)
{
    g_autofree char *state = NULL;
    g_autofree char *uri = NULL;
    int fd = g_file_open_tmp("fmss-snapshot-XXXXXX", &state, NULL);
    bool completed = false;
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    uri = g_strdup_printf("file:%s", state);
    qtest_qmp_assert_success(qts, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    for (unsigned i = 0; i < 10000; i++) {
        QDict *reply = qtest_qmp(qts, "{ 'execute': 'query-migrate' }");
        QDict *result = qdict_get_qdict(reply, "return");
        const char *status = qdict_get_try_str(result, "status");
        if (status && (!strcmp(status, "completed") || !strcmp(status, "failed"))) {
            completed = !strcmp(status, "completed");
            qobject_unref(reply);
            unlink(state);
            return completed;
        }
        qobject_unref(reply);
        g_usleep(1000);
    }
    g_assert_not_reached();
    return false;
}

static void empty_snapshot(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    g_assert_true(migrate(qts));
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void programmed_snapshot_refused(void)
{
    char *overlay;
    uint8_t data[4096], spare[12] = { 0 };
    QTestState *qts = start_board(&overlay);
    memset(data, 0x5a, sizeof(data));
    qtest_memwrite(qts, RAM + 0x10000, data, sizeof(data));
    qtest_memwrite(qts, RAM + 0x2000, spare, sizeof(spare));
    qtest_writel(qts, RAM, 1);       /* chip 0, one physical program */
    qtest_writel(qts, RAM + 4, 5);
    qtest_writel(qts, RAM + 8, 0);   /* sequencer script terminator */
    qtest_writel(qts, RAM + 0x1000, RAM + 0x10000);
    qtest_writel(qts, RAM + 0x1004, RAM + 0x10800);
    qtest_writel(qts, FMSS + 0xd10, RAM);
    qtest_writel(qts, FMSS + 0xd20, RAM + 0x1000);
    qtest_writel(qts, FMSS + 0xd1c, RAM + 0x2000);
    qtest_writel(qts, FMSS + 0xd30, 0xa02);
    qtest_writel(qts, FMSS + 0xd38, 1);
    g_assert_false(migrate(qts));
    qtest_quit(qts);
    g_autofree char *page = g_strdup_printf("%s/cs0/5.page", overlay);
    g_autofree char *chip = g_strdup_printf("%s/cs0", overlay);
    g_assert_true(g_file_test(page, G_FILE_TEST_EXISTS));
    unlink(page);
    rmdir(chip);
    rmdir(overlay);
    g_free(overlay);
}

int main(int argc, char **argv)
{
    g_autofree char *rom = g_malloc0(131072);
    g_autofree char *nor = g_malloc0(1048576);
    int fd, result;
    g_setenv("FMSS_PHYSICAL", "1", true);
    fd = g_file_open_tmp("fmss-rom-XXXXXX", &rom_path, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("fmss-nor-XXXXXX", &nor_path, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom_path, rom, 131072, NULL));
    g_assert_true(g_file_set_contents(nor_path, nor, 1048576, NULL));
    nand_path = g_dir_make_tmp("fmss-base-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/fmss/empty-snapshot", empty_snapshot);
    qtest_add_func("/ipod/fmss/programmed-snapshot-refused", programmed_snapshot_refused);
    result = g_test_run();
    unlink(rom_path); unlink(nor_path); rmdir(nand_path);
    g_free(rom_path); g_free(nor_path); g_free(nand_path);
    return result;
}
