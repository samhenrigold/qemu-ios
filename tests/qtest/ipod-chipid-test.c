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

/* CPFM is decoded by the stock ROM from separate immutable fuse fields. */
static void security_profiles(void)
{
    const char *profiles[] = { "retail", "secure-development", "insecure-development" };
    const unsigned cpfm[] = { 3, 1, 0 };
    for (unsigned i = 0; i < G_N_ELEMENTS(profiles); i++) {
        QTestState *q = qtest_initf(
            "-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s,security-profile=%s,forge-sigcheck=off "
            "-display none -audio driver=none -nic none "
            "-global driver=ipodtouch.chipid,property=word2,value=0x87200005",
            rom, nor, nand, profiles[i]);
        for (unsigned reset = 0; reset < 2; reset++) {
            uint32_t prod = qtest_readl(q, CHIPID + 4);
            uint32_t info = qtest_readl(q, CHIPID + 8);
            unsigned production = (prod >> 5) & 1;
            unsigned secure = ((info >> 1) & 1) | production;
            g_assert_cmpuint(secure | (production << 1), ==, cpfm[i]);
            /* Domain 1, chip 8720, 24MHz source stay independent of CPFM. */
            g_assert_cmphex(info & ~2u, ==, 0x87200005);
            g_assert_cmphex(prod & ~0x20u, ==, 0);
            qtest_writel(q, CHIPID + 4, ~prod);
            qtest_writel(q, CHIPID + 8, ~info);
            g_assert_cmphex(qtest_readl(q, CHIPID + 4), ==, prod);
            g_assert_cmphex(qtest_readl(q, CHIPID + 8), ==, info);
            qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
        }
        QDict *reply = qtest_qmp(q, "{'execute':'qom-set','arguments':"
            "{'path':'/machine','property':'security-profile','value':'retail'}}");
        g_assert_nonnull(qdict_get(reply, "error"));
        qobject_unref(reply);
        qtest_quit(q);
    }
}

static void migration_completed(QTestState *q)
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

static QTestState *migration_board(const char *profile, bool incoming, bool n45,
                                  const char *n45_rom, bool different_identity)
{
    if (n45) {
        return qtest_initf("-machine iPod-Touch-1G,bootrom=%s,nand=%s,x-rom-boot=on,wifi=off "
            "-drive if=pflash,format=raw,file=%s -display none -audio driver=none -nic none %s %s",
            n45_rom, nand, nor, incoming ? "-incoming defer" : "",
            different_identity ? "-global driver=ipodtouch.chipid,property=word3,value=1" : "");
    }
    return qtest_initf("-machine iPod-Touch,bootrom=%s,nand=%s,nor=%s,security-profile=%s "
        "-display none -audio driver=none -nic none %s", rom, nand, nor, profile,
        incoming ? "-incoming defer" : "");
}

static void migrated_fuses_case(bool n45, bool mismatch)
{
    g_autofree char *state = NULL;
    g_autofree char *n45_rom = NULL;
    int fd = g_file_open_tmp("chipid-state-XXXXXX", &state, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    if (n45) {
        fd = g_file_open_tmp("chipid-n45-rom-XXXXXX", &n45_rom, NULL);
        g_assert_cmpint(fd, >=, 0); close(fd);
        g_autofree char *zero = g_malloc0(65536);
        g_assert_true(g_file_set_contents(n45_rom, zero, 65536, NULL));
    }
    QTestState *from = migration_board("secure-development", false, n45, n45_rom, false);
    qtest_qmp_assert_success(from, "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    migration_completed(from);
    qtest_quit(from);
    QTestState *to = migration_board(mismatch ? "retail" : "secure-development", true,
                                    n45, n45_rom, mismatch);
    if (mismatch) {
        /* Incoming fuse mismatch is fatal; do not reinterpret EOF as success. */
        qtest_set_expected_status(to, EXIT_FAILURE);
        qtest_qmp_send(to, "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        qtest_wait_qemu(to);
    } else {
        qtest_qmp_assert_success(to, "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
        migration_completed(to);
        g_assert_cmphex(qtest_readl(to, n45 ? 0x3e500004 : CHIPID + 4), ==, n45 ? 0x02000000 : 0);
        g_assert_cmphex(qtest_readl(to, n45 ? 0x3e500008 : CHIPID + 8), ==, n45 ? 0 : 0x87200006);
    }
    qtest_quit(to);
    unlink(state);
    if (n45_rom) { unlink(n45_rom); }
}
static void migrated_n72(void) { migrated_fuses_case(false, false); }
static void refused_n72(void) { migrated_fuses_case(false, true); }
static void migrated_n45(void) { migrated_fuses_case(true, false); }
static void refused_n45(void) { migrated_fuses_case(true, true); }

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
    qtest_add_func("/ipod/chipid/security-profiles-read-only-reset", security_profiles);
    qtest_add_func("/ipod/chipid/migration-n72-same-profile", migrated_n72);
    qtest_add_func("/ipod/chipid/migration-n72-profile-mismatch", refused_n72);
    qtest_add_func("/ipod/chipid/migration-n45-same-fuses", migrated_n45);
    qtest_add_func("/ipod/chipid/migration-n45-fuse-mismatch", refused_n45);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
