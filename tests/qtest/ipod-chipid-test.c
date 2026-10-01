/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ECID inputs on the production N72 board; fuse writes and reset are inert. */
#include "qemu/osdep.h"
#include "libqtest.h"

#define CHIPID 0x3d100000ULL
static char *rom, *nor, *nand;

static QTestState *start_board(bool provisioned)
{
    return qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s "
                      "-display none -audio driver=none -nic none %s", rom, nor, nand,
                      provisioned ? "-global driver=ipodtouch.chipid,property=word3,value=0xd6e54321 "
                                    "-global driver=ipodtouch.chipid,property=word4,value=0x000002a7" : "");
}

static void default_fuses(void)
{
    QTestState *q = start_board(false);
    g_assert_cmphex(qtest_readl(q, CHIPID + 4), ==, 0x20);
    g_assert_cmphex(qtest_readl(q, CHIPID + 8), ==, 0x87200004);
    g_assert_cmphex(qtest_readl(q, CHIPID + 12), ==, 0);
    g_assert_cmphex(qtest_readl(q, CHIPID + 16), ==, 0);
    qtest_quit(q);
}

static void provisioned_read_only_fuses(void)
{
    QTestState *q = start_board(true);
    for (unsigned i = 0; i < 2; i++) {
        /* Identity must not alter production/security/chip-revision fuses. */
        g_assert_cmphex(qtest_readl(q, CHIPID + 4), ==, 0x20);
        g_assert_cmphex(qtest_readl(q, CHIPID + 8), ==, 0x87200004);
        g_assert_cmphex(qtest_readl(q, CHIPID + 12), ==, 0xd6e54321);
        g_assert_cmphex(qtest_readl(q, CHIPID + 16), ==, 0x2a7);
        qtest_writel(q, CHIPID + 12, 0);
        qtest_writel(q, CHIPID + 16, 0xffffffff);
        qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    }
    qtest_quit(q);
}

static void machine_ecid(void)
{
    QTestState *q = qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s,ecid=0xa86437a9d7 "
                              "-display none -audio driver=none -nic none "
                              "-global driver=ipodtouch.chipid,property=word4,value=0xabcd2400",
                              rom, nor, nand);
    for (unsigned i = 0; i < 2; i++) {
        g_assert_cmphex(qtest_readl(q, CHIPID + 12), ==, 0xd6e54321);
        g_assert_cmphex(qtest_readl(q, CHIPID + 16), ==, 0xabcd26a7);
        g_assert_cmphex(qtest_readl(q, CHIPID + 4), ==, 0x20);
        g_assert_cmphex(qtest_readl(q, CHIPID + 8), ==, 0x87200004);
        qtest_writel(q, CHIPID + 12, 0);
        qtest_writel(q, CHIPID + 16, 0);
        qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    }
    QDict *reply = qtest_qmp(q, "{'execute':'qom-set','arguments':"
                            "{'path':'/machine','property':'ecid','value':1}}");
    g_assert_nonnull(qdict_get(reply, "error"));
    qobject_unref(reply);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;
    fd = g_file_open_tmp("n72-chipid-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n72-chipid-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("n72-chipid-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/chipid/default-fuses", default_fuses);
    qtest_add_func("/ipod/chipid/provisioned-read-only-reset", provisioned_read_only_fuses);
    qtest_add_func("/ipod/chipid/machine-ecid-read-only-reset", machine_ecid);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
