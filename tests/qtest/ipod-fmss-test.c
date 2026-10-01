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

static bool migrate(QTestState *qts, char **saved)
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
            if (completed && saved) {
                *saved = g_steal_pointer(&state);
            } else {
                unlink(state);
            }
            return completed;
        }
        qobject_unref(reply);
        g_usleep(1000);
    }
    g_assert_not_reached();
    return false;
}

static void expect_startup_failure(char **argv, const char *error_text)
{
    GPid pid;
    int status, errfd;
    bool exited = false;
    g_assert_true(g_spawn_async_with_pipes(NULL, argv, NULL, G_SPAWN_DO_NOT_REAP_CHILD,
                 NULL, NULL, &pid, NULL, NULL, &errfd, NULL));
    for (unsigned i = 0; i < 2000; i++) {
        if (waitpid(pid, &status, WNOHANG) == pid) {
            exited = true;
            break;
        }
        g_usleep(1000);
    }
    if (!exited) {
        kill(pid, SIGTERM);
        waitpid(pid, &status, 0);
    }
    char message[4096] = { 0 };
    ssize_t n = read(errfd, message, sizeof(message) - 1);
    close(errfd);
    g_spawn_close_pid(pid);
    g_assert_true(exited);
    g_assert_cmpint(n, >, 0);
    g_assert_true(WIFEXITED(status) && WEXITSTATUS(status) != 0);
    if (!strstr(message, error_text)) {
        g_test_message("Unexpected QEMU startup error: %s", message);
    }
    g_assert_nonnull(strstr(message, error_text));
}

static void snapshot_mode_mismatch(void)
{
    char *overlay;
    g_autofree char *state = NULL;
    g_setenv("FMSS_PHYSICAL", "1", true);
    g_unsetenv("FMSS_ERASE");
    QTestState *qts = start_board(&overlay);
    g_assert_true(migrate(qts, &state));
    qtest_quit(qts);
    g_unsetenv("FMSS_PHYSICAL");
    g_autofree char *machine = g_strdup_printf(
        "iPod-Touch,bootrom=%s,nor=%s,nand=%s,nandrw=%s",
        rom_path, nor_path, nand_path, overlay);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    char *argv[] = { getenv("QTEST_QEMU_BINARY"), "-machine", machine,
        "-accel", "qtest", "-incoming", uri, "-S", "-display", "none",
        "-audio", "driver=none", "-nic", "none", "-monitor", "none",
        "-serial", "none", NULL };
    expect_startup_failure(argv, "FMSS startup read/write modes differ");
    unlink(state);
    rmdir(overlay);
    g_free(overlay);
    g_setenv("FMSS_PHYSICAL", "1", true);
}

static void empty_snapshot(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    g_assert_true(migrate(qts, NULL));
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void program(QTestState *qts, unsigned cs, unsigned page,
                    const uint8_t *data, const uint8_t *spare)
{
    qtest_memwrite(qts, RAM + 0x10000, data, 4096);
    qtest_memwrite(qts, RAM + 0x2000, spare, 12);
    qtest_writel(qts, RAM, 1u << cs);
    qtest_writel(qts, RAM + 4, page);
    qtest_writel(qts, RAM + 8, 0);
    qtest_writel(qts, RAM + 0x1000, RAM + 0x10000);
    qtest_writel(qts, RAM + 0x1004, RAM + 0x10800);
    qtest_writel(qts, FMSS + 0xd10, RAM);
    qtest_writel(qts, FMSS + 0xd20, RAM + 0x1000);
    qtest_writel(qts, FMSS + 0xd1c, RAM + 0x2000);
    qtest_writel(qts, FMSS + 0xd30, 0xa02);
    qtest_writel(qts, FMSS + 0xd38, 1);
}

static void read_page(QTestState *qts, unsigned cs, unsigned page,
                      uint8_t *data, uint8_t *spare)
{
    qtest_writel(qts, RAM + 0x80, page);
    qtest_writel(qts, RAM + 0x84, 1u << cs);
    qtest_writel(qts, RAM + 0x1800, RAM + 0x30000);
    qtest_writel(qts, RAM + 0x1804, RAM + 0x30800);
    qtest_writel(qts, FMSS + 0xd0c, RAM + 0x80);
    qtest_writel(qts, FMSS + 0xd10, RAM + 0x84);
    qtest_writel(qts, FMSS + 0xd18, 1);
    qtest_writel(qts, FMSS + 0xd20, RAM + 0x1800);
    qtest_writel(qts, FMSS + 0xd1c, RAM + 0x2800);
    qtest_writel(qts, FMSS + 0xd30, 0xa01);
    qtest_writel(qts, FMSS + 0xd38, 1);
    qtest_memread(qts, RAM + 0x30000, data, 4096);
    qtest_memread(qts, RAM + 0x2800, spare, 12);
}

static void programmed_snapshot(bool physical)
{
    char *overlay;
    g_autofree char *state = NULL;
    uint8_t data[4096], spare[12] = { 0 }, back[4096], back_spare[12];
    uint8_t pristine[4096], pristine_spare[12];
    uint8_t erased[4096], erased_spare[12];
    g_autofree char *base_chip = g_build_filename(nand_path, "cs0", NULL);
    g_autofree char *base_page = g_build_filename(base_chip, "6.page", NULL);
    if (physical) {
        /* A real base page distinguishes an erased cache hit from a disk
         * fallback. Legacy FMSS erased pages retain its existing zero encoding. */
        uint8_t record[4160];
        memset(record, 0xa3, sizeof(record));
        g_assert_cmpint(g_mkdir_with_parents(base_chip, 0755), ==, 0);
        g_assert_true(g_file_set_contents(base_page, (char *)record, sizeof(record), NULL));
        g_setenv("FMSS_PHYSICAL", "1", true);
        g_setenv("FMSS_ERASE", "1", true);
    } else {
        g_unsetenv("FMSS_PHYSICAL");
        g_unsetenv("FMSS_ERASE");
    }
    QTestState *qts = start_board(&overlay);
    read_page(qts, 0, 5, pristine, pristine_spare);
    /* Logical zero is bookkeeping and has no generated disk destination.
     * The generated-mode page therefore exists ONLY in phys_pages. */
    memset(data, 0x5a, sizeof(data));
    spare[8] = 0x79;
    program(qts, 0, 5, data, spare);
    data[0] = 0xc3;
    spare[8] = 0x80;
    program(qts, 3, 129, data, spare);
    read_page(qts, 3, 129, back, back_spare);
    g_assert_cmpmem(back, sizeof(back), data, sizeof(data));
    g_assert_cmpmem(back_spare, sizeof(back_spare), spare, sizeof(spare));
    if (physical) {
        read_page(qts, 0, 6, erased, erased_spare);
        g_assert_cmpint(erased[0], !=, 0xa3);
    }
    qtest_writel(qts, FMSS + 0xd28, 2);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd28), ==, 2);
    qtest_writel(qts, FMSS + 0xd4c, 0x20011000);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd4c), ==, 0x20011000);
    g_assert_true(migrate(qts, &state));
    qtest_quit(qts);
    if (physical) {
        /* Prove the serialized erase map (including direct key zero), rather
         * than reconstructing it from its backing marker file. This is an
         * isolated firmware-free fixture, not a valid user snapshot edit. */
        g_autofree char *marker = g_build_filename(overlay, "cs0", "blk0.erased", NULL);
        g_assert_cmpint(unlink(marker), ==, 0);
    }

    qts = qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s,nandrw=%s "
                      "-display none -audio driver=none -nic none -incoming defer",
                      rom_path, nor_path, nand_path, overlay);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    qtest_qmp_assert_success(qts, "{ 'execute': 'migrate-incoming', "
                                "'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(qts, "RESUME");
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd28), ==, 2);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd4c), ==, 0x20011000);
    if (physical) {
        read_page(qts, 0, 6, back, back_spare);
        g_assert_cmpmem(back, sizeof(back), erased, sizeof(erased));
        g_assert_cmpmem(back_spare, sizeof(back_spare), erased_spare, sizeof(erased_spare));
    }
    read_page(qts, 3, 129, back, back_spare);
    g_assert_cmpmem(back, sizeof(back), data, sizeof(data));
    g_assert_cmpmem(back_spare, sizeof(back_spare), spare, sizeof(spare));
    data[0] = 0x5a;
    spare[8] = 0x79;
    read_page(qts, 0, 5, back, back_spare);
    g_assert_cmpmem(back, sizeof(back), data, sizeof(data));
    g_assert_cmpmem(back_spare, sizeof(back_spare), spare, sizeof(spare));
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd28), ==, 0);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd4c), ==, 0);
    read_page(qts, 0, 5, back, back_spare);
    if (physical) {
        g_assert_cmpmem(back, sizeof(back), data, sizeof(data));
    } else {
        /* A cold boot rebuilds the generated FTL; its temporary mapping must
         * not shadow the new physical view. */
        g_assert_cmpmem(back, sizeof(back), pristine, sizeof(pristine));
        g_assert_cmpmem(back_spare, sizeof(back_spare), pristine_spare, sizeof(pristine_spare));
    }
    qtest_quit(qts);
    unlink(state);
    if (physical) {
        unlink(base_page);
        rmdir(base_chip);
    }
    for (unsigned cs = 0; cs < 4; cs++) {
        g_autofree char *chip = g_strdup_printf("%s/cs%u", overlay, cs);
        GDir *dir = g_dir_open(chip, 0, NULL);
        if (dir) {
            const char *name;
            while ((name = g_dir_read_name(dir))) {
                g_autofree char *path = g_build_filename(chip, name, NULL);
                unlink(path);
            }
            g_dir_close(dir);
        }
        rmdir(chip);
    }
    rmdir(overlay);
    g_free(overlay);
    g_setenv("FMSS_PHYSICAL", "1", true);
    g_unsetenv("FMSS_ERASE");
}

static void physical_snapshot(void)
{
    programmed_snapshot(true);
}

static void generated_snapshot(void)
{
    programmed_snapshot(false);
}

/* Exercise the real sequencer and QEMU guest RAM, not a replacement DMA stub. */
static void register_copy(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    static const uint32_t program[] = {
        0x05030000, 0x89abcdef, 0x05040000, 0x13579bdf,
        0x06040003, 0, 0x05070000, RAM + 0x1000,
        0x11040007, 0, 0x05010000, 0x89abcdef,
        0x06010001, 0,
        0x0c070007, 4, 0x11010007, 0,
        0x05030000, 0, 0x06040003, 0,
        0x0c070007, 4, 0x11040007, 0,
        0x05030000, 4, 0x05040000, 8,
        0x06020003, 0, 0x06030004, 0,
        0x0c070007, 4, 0x11020007, 0,
        0x0c070007, 4, 0x11030007, 0, 0, 0
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
        qtest_writel(qts, RAM + 4 * i, program[i]);
    }
    qtest_writel(qts, FMSS + 0xc04, RAM);
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, 0x89abcdef);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x1004), ==, 0x89abcdef);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x1008), ==, 0);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x100c), ==, 4);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x1010), ==, 8);
    qtest_writel(qts, FMSS + 0xc00, 8);

    static const uint32_t rejected[] = {
        0x05030000, 7, 0x05070000, RAM + 0x1100,
        0x06040003, 1, 0x11040007, 0, 0, 0
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(rejected); i++) {
        qtest_writel(qts, RAM + 4 * i, rejected[i]);
    }
    qtest_writel(qts, RAM + 0x1100, 0x12345678);
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x1100), ==, 0x12345678);
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void descriptor_load(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    static const uint32_t program[] = {
        0x05000000, RAM + 0x2000, 0x05010000, 0xdeadc0de,
        0x03010000, 0, 0x05060000, RAM + 0x3000,
        0x11010006, 0, 0x0c000000, 4,
        0x05020000, 0xdeadc0de, 0x03020000, 0,
        0x0c060006, 4, 0x11020006, 0,
        0x05010000, RAM + 0x2008, 0x05070000, 0xdeadc0de,
        0x03070001, 0, 0x0c060006, 4,
        0x11070006, 0, 0x0c010001, 4,
        0x05000000, 0xdeadc0de, 0x03000001, 0,
        0x0c060006, 4, 0x11000006, 0, 0, 0
    };
    static const uint8_t values[] = {
        1, 0x23, 0x45, 0x67, 0xff, 0xee, 0xdd, 0xcc,
        0, 0, 0, 0, 0x78, 0x56, 0x34, 0x12
    };
    static const uint32_t expected[] = {
        0x67452301, 0xccddeeff, 0, 0x12345678
    };
    uint8_t unchanged[sizeof(values)];
    qtest_memwrite(qts, RAM + 0x2000, values, sizeof(values));
    for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
        qtest_writel(qts, RAM + 4 * i, program[i]);
    }
    qtest_writel(qts, FMSS + 0xc04, RAM);
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    for (unsigned i = 0; i < G_N_ELEMENTS(expected); i++) {
        g_assert_cmphex(qtest_readl(qts, RAM + 0x3000 + 4 * i), ==,
                        expected[i]);
    }
    qtest_memread(qts, RAM + 0x2000, unchanged, sizeof(unchanged));
    g_assert_cmpmem(unchanged, sizeof(unchanged), values, sizeof(values));
    qtest_writel(qts, FMSS + 0xc00, 8);

    uint32_t rejected[] = {
        0x05000000, 0xfffffff0, 0x05020000, 7,
        0x03020000, 0, 0x05060000, RAM + 0x3000,
        0x11020006, 0, 0, 0
    };
    for (unsigned immediate = 0; immediate < 2; immediate++) {
        /* Physical fffffff0 is unassigned on this board. Decode failure must
         * stop before the later store, rather than fabricate a descriptor.
         * Nonzero immediate is unsupported and must not attempt DMA at all. */
        rejected[5] = immediate;
        for (unsigned i = 0; i < G_N_ELEMENTS(rejected); i++) {
            qtest_writel(qts, RAM + 4 * i, rejected[i]);
        }
        qtest_writel(qts, RAM + 0x3000, 0xabcddcba);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x3000), ==, 0xabcddcba);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    qtest_quit(qts);
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
    qtest_add_func("/ipod/fmss/mode-mismatch", snapshot_mode_mismatch);
    qtest_add_func("/ipod/fmss/physical-snapshot", physical_snapshot);
    qtest_add_func("/ipod/fmss/generated-snapshot", generated_snapshot);
    qtest_add_func("/ipod/fmss/register-copy", register_copy);
    qtest_add_func("/ipod/fmss/descriptor-load", descriptor_load);
    result = g_test_run();
    unlink(rom_path); unlink(nor_path); rmdir(nand_path);
    g_free(rom_path); g_free(nor_path); g_free(nand_path);
    return result;
}
