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
                       "-display none -audio driver=none -nic none -d unimp",
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


/* These are the controller's 12 visible metadata bytes, not raw NAND OOB. */
static void assert_blank(const uint8_t *data, const uint8_t *spare, bool erased)
{
    for (unsigned i = 0; i < 4096; i++) {
        g_assert_cmphex(data[i], ==, erased ? 0xff : 0);
    }
    for (unsigned i = 0; i < 12; i++) {
        g_assert_cmphex(spare[i], ==, erased ? 0xff :
                        ((i == 8 || i == 10) ? 0xff : 0));
    }
}

static void blank_read_modes(bool physical, bool packed)
{
    char *overlay, *saved_nand = nand_path;
    g_autofree char *image = NULL;
    g_autofree char *chip = g_build_filename(nand_path, "cs0", NULL);
    g_autofree char *base_page = g_build_filename(chip, "0.page", NULL);
    g_autofree char *short_page = g_build_filename(chip, "2.page", NULL);
    uint8_t data[4096], spare[12], record[4160];
    memset(record, 0x5a, sizeof(record));
    if (packed) {
        int fd = g_file_open_tmp("fmss-packed-XXXXXX", &image, NULL);
        g_assert_cmpint(fd, >=, 0); close(fd);
        /* Index0=data, index1=hole, index2=invalid. Index3 is out of range.
         * Invalid cases retain fallback bytes; this does not certify their
         * current permissive error policy as correct physical behavior. */
        uint8_t bytes[24 + 3 * 4 + sizeof(record)] = { 0 };
        static const uint32_t words[] = { 4096, 64, 1, 3, 1, 0, 2 };
        memcpy(bytes, "ITNAND01", 8);
        for (unsigned i = 0; i < G_N_ELEMENTS(words); i++) {
            uint32_t little = GUINT32_TO_LE(words[i]);
            memcpy(bytes + 8 + 4 * i, &little, sizeof(little));
        }
        memcpy(bytes + 24 + 3 * 4, record, sizeof(record));
        g_assert_true(g_file_set_contents(image, (char *)bytes, sizeof(bytes), NULL));
        nand_path = image;
    } else {
        g_assert_cmpint(g_mkdir_with_parents(chip, 0755), ==, 0);
        g_assert_true(g_file_set_contents(base_page, (char *)record, sizeof(record), NULL));
        /* Short files deliberately keep the old zero-padding policy. */
        g_assert_true(g_file_set_contents(short_page, (char *)record, 20, NULL));
    }
    if (physical) {
        g_setenv("FMSS_PHYSICAL", "1", true);
    } else {
        g_unsetenv("FMSS_PHYSICAL");
    }
    g_setenv("FMSS_ERASE", "1", true);
    QTestState *qts = start_board(&overlay);
    read_page(qts, 0, 0, data, spare);
    g_assert_cmpmem(data, sizeof(data), record, sizeof(data));
    g_assert_cmpmem(spare, sizeof(spare), record + 4096, sizeof(spare));
    read_page(qts, 0, 1, data, spare);
    assert_blank(data, spare, physical);
    if (packed) {
        read_page(qts, 0, 2, data, spare);
        assert_blank(data, spare, false);
        read_page(qts, 0, 3, data, spare);
        assert_blank(data, spare, false);
        read_page(qts, 1, 0, data, spare);
        assert_blank(data, spare, false);
    } else {
        read_page(qts, 0, 2, data, spare);
        for (unsigned i = 0; i < sizeof(data); i++) {
            g_assert_cmphex(data[i], ==, i < 20 ? 0x5a : 0);
        }
        for (unsigned i = 0; i < sizeof(spare); i++) {
            g_assert_cmphex(spare[i], ==, 0);
        }
    }
    /* A durable erased marker shadows a non-erased base record on either
     * backend. Programmed-page replay is exercised by the snapshot test. */
    g_autofree char *overlay_chip = g_build_filename(overlay, "cs0", NULL);
    g_autofree char *marker = g_build_filename(overlay_chip, "blk0.erased", NULL);
    g_assert_cmpint(g_mkdir_with_parents(overlay_chip, 0755), ==, 0);
    g_assert_true(g_file_set_contents(marker, "", 0, NULL));
    read_page(qts, 0, 0, data, spare);
    assert_blank(data, spare, physical);
    qtest_quit(qts);
    unlink(marker);rmdir(overlay_chip);rmdir(overlay);g_free(overlay);
    nand_path = saved_nand;
    if (packed) {
        unlink(image);
    } else {
        unlink(base_page);unlink(short_page);rmdir(chip);
    }
    g_setenv("FMSS_PHYSICAL", "1", true);
    g_unsetenv("FMSS_ERASE");
}


static void nonabsence_open_fallback(void)
{
    char *overlay, *saved_nand = nand_path;
    g_autofree char *badbase = NULL;
    int fd = g_file_open_tmp("fmss-not-directory-XXXXXX", &badbase, NULL);
    uint8_t data[4096], spare[12], record[4160];
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(badbase, "x", 1, NULL));
    g_setenv("FMSS_PHYSICAL", "1", true);
    g_setenv("FMSS_ERASE", "1", true);
    nand_path = badbase;
    QTestState *qts = start_board(&overlay);
    /* Actual ENOTDIR base open, not a physical missing page. Preserve old
     * permissive fallback bytes without certifying that error policy. */
    read_page(qts, 0, 129, data, spare);
    assert_blank(data, spare, false);
    qtest_quit(qts);rmdir(overlay);g_free(overlay);
    nand_path = saved_nand;unlink(badbase);

    qts = start_board(&overlay);
    g_autofree char *chip = g_build_filename(overlay, "cs0", NULL);
    g_autofree char *page = g_build_filename(chip, "129.page", NULL);
    memset(record, 0x79, sizeof(record));
    g_assert_cmpint(g_mkdir_with_parents(chip, 0755), ==, 0);
    g_assert_true(g_file_set_contents(page, (char *)record, sizeof(record), NULL));
    /* First read indexes the actual overlay. A subsequent path failure must
     * not become FF merely because the base page is legitimately absent. */
    read_page(qts, 0, 129, data, spare);
    g_assert_cmpmem(data, sizeof(data), record, sizeof(data));
    g_assert_cmpint(unlink(page), ==, 0);g_assert_cmpint(rmdir(chip), ==, 0);
    g_assert_true(g_file_set_contents(chip, "x", 1, NULL));
    read_page(qts, 0, 129, data, spare);
    assert_blank(data, spare, false);
    qtest_quit(qts);unlink(chip);rmdir(overlay);g_free(overlay);
    g_unsetenv("FMSS_ERASE");
}

static void directory_physical(void) { blank_read_modes(true, false); }
static void directory_generated(void) { blank_read_modes(false, false); }
static void packed_physical(void) { blank_read_modes(true, true); }
static void packed_generated(void) { blank_read_modes(false, true); }

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
         * fallback. Explicit physical erased pages must expose erased bytes. */
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
        assert_blank(erased, erased_spare, true);
    }
    qtest_writel(qts, FMSS + 0xd28, 2);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd28), ==, 2);
    qtest_writel(qts, FMSS + 0xd38, 0x8000000c);
    qtest_writel(qts, FMSS + 0xd34, 0x80000016);
    qtest_writel(qts, FMSS + 0xd48, 0x30012000);
    qtest_writel(qts, FMSS + 0xd4c, 0x20011000);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd38), ==, 0x8000000c);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd34), ==, 0x80000016);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd48), ==, 0x30012000);
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
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd38), ==, 0x8000000c);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd34), ==, 0x80000016);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd48), ==, 0x30012000);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd4c), ==, 0x20011000);
    if (physical) {
        read_page(qts, 0, 6, back, back_spare);
        assert_blank(back, back_spare, true);
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
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd38), ==, 0);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd34), ==, 0);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd48), ==, 0);
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

static void logical_and(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    uint32_t program[] = {
        0x05000000, 1, 0x05040000, 1,
        0x0a000004, 0, 0x05070000, RAM + 0x1000,
        0x11000007, 0, 0, 0
    };
    static const uint32_t inputs[][2] = {
        {1, 1}, {1, 2}, {0xff00ff00, 0x0ff00ff0},
        {0, 0xffffffff}, {0xffffffff, 0}, {0x80000000, 0x80000001}
    };
    for (unsigned j = 0; j < G_N_ELEMENTS(inputs); j++) {
        program[1] = inputs[j][0];
        program[3] = inputs[j][1];
        for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
            qtest_writel(qts, RAM + 4 * i, program[i]);
        }
        qtest_writel(qts, FMSS + 0xc04, RAM);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==,
                        inputs[j][0] & inputs[j][1]);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    /* Actual nonzero-immediate form0A010000/1 has distinct destination/source. */
    static const uint32_t immediate[] = {
        0x05000000, 0x12345679, 0x05010000, 0xdeadbeee,
        0x0a010000, 1, 0x05070000, RAM + 0x1000,
        0x11010007, 0, 0, 0
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(immediate); i++) {
        qtest_writel(qts, RAM + 4 * i, immediate[i]);
    }
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, 1);
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void read_id_chip_selection(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    uint32_t program[] = {
        0x05000000, 0, 0x02000000, 0,
        0x01000008, 0x90,
        0x04010060, 0xffffffff, 0x04030064, 0xffffffff,
        0x05020000, RAM + 0x1000,
        0x11010002, 0, 0x0c020002, 4, 0x11030002, 0, 0, 0
    };
    static const struct {
        unsigned selector;
        uint32_t expected;
    } cases[] = {
        {0, 0},         /* No selected chip: ctz32(0) returns 32. */
        {3, 0},         /* Selecting multiple chips is not a single CE. */
        {0x10, 0},      /* CE4 is absent on this board. */
        {1, 0xb614d5ad}, {2, 0xb614d5ad},
        {4, 0xb614d5ad}, {8, 0xb614d5ad}
    };
    for (unsigned j = 0; j < G_N_ELEMENTS(cases); j++) {
        program[1] = (cases[j].selector << 1) | 1;
        for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
            qtest_writel(qts, RAM + 4 * i, program[i]);
        }
        /* Sentinel proves both READ-ID words reached RAM through opcode11. */
        qtest_writel(qts, RAM + 0x1000, 0xdeadbeef);
        qtest_writel(qts, RAM + 0x1004, 0xdeadbeef);
        qtest_writel(qts, FMSS + 0xc04, RAM);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==,
                        cases[j].expected);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1004), ==, 0);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void observed_right_shift(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    uint32_t program[] = {
        0x05010000, 0, 0x05000000, 0x76543210,
        0x14000001, 16, 0x05060000, RAM + 0x1000,
        0x11000006, 0,
        0x05070000, 0, 0x05010000, 0x76543210,
        0x14010007, 16, 0x0c060006, 4,
        0x11010006, 0, 0, 0
    };
    static const uint32_t values[] = {
        0, 0x12345, 0x00ffffff, 0x12345678, 0x7fffffff
    };
    /* Actual bulk/read operand forms; all sources have bit31 clear, so these
     * observations do not establish logical versus arithmetic signedness. */
    for (unsigned j = 0; j < G_N_ELEMENTS(values); j++) {
        program[1] = values[j];
        program[11] = values[j];
        for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
            qtest_writel(qts, RAM + 4 * i, program[i]);
        }
        qtest_writel(qts, RAM + 0x1000, 0xdeadbeef);
        qtest_writel(qts, RAM + 0x1004, 0xdeadbeef);
        qtest_writel(qts, FMSS + 0xc04, RAM);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, values[j] >> 16);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1004), ==, values[j] >> 16);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    uint32_t rejected[] = {
        0x05010000, 0x12345678, 0x05000000, 7,
        0x14000001, 0, 0x05060000, RAM + 0x1000,
        0x11000006, 0, 0, 0
    };
    static const struct {
        uint32_t source;
        unsigned immediate;
    } unmeasured[] = {
        {0x12345678, 0}, {0x12345678, 1}, {0x12345678, 15},
        {0x12345678, 17}, {0x12345678, 31}, {0x12345678, 32},
        {0x80000000, 16}, {0x89abcdef, 16}, {0xffffffff, 16}
    };
    for (unsigned j = 0; j < G_N_ELEMENTS(unmeasured); j++) {
        rejected[1] = unmeasured[j].source;
        rejected[5] = unmeasured[j].immediate;
        for (unsigned i = 0; i < G_N_ELEMENTS(rejected); i++) {
            qtest_writel(qts, RAM + 4 * i, rejected[i]);
        }
        qtest_writel(qts, RAM + 0x1000, 0xabcddcba);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, 0xabcddcba);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void parameter_latches(void)
{
    char *overlay = NULL;
    QTestState *qts = start_board(&overlay);
    const uint32_t d34[] = { 0, 0x16, 0x80000000, 0xffffffff };
    const uint32_t d48[] = { 0xffffffff, 0x20011000, 0, 0x80000000 };
    const uint32_t script[] = {
        0x04000d34, 0xffffffff, 0x02000030, 0,
        0x04010030, 0xffffffff, 0x05020000, RAM + 0x2000, 0x11010002, 0,
        0x04000d48, 0xffffffff, 0x0b000000, 0x801, 0x02000000, 0,
        0x04010000, 0xffffffff, 0x0c020002, 4, 0x11010002, 0, 0, 0
    };
    /* D34 uses the observed stock read/write pair. D48 uses immediate
     * OR801 as an observation oracle, not the unresolved stock register-OR
     * form. FMC shadow observations do not claim NAND/ECC execution. */
    for (unsigned i = 0; i < G_N_ELEMENTS(script); i++) {
        qtest_writel(qts, RAM + 4 * i, script[i]);
    }
    qtest_writel(qts, FMSS + 0xc04, RAM);
    for (unsigned i = 0; i < G_N_ELEMENTS(d34); i++) {
        qtest_writel(qts, FMSS + 0xd34, d34[i]);
        qtest_writel(qts, FMSS + 0xd48, d48[i]);
        qtest_writel(qts, FMSS + 0xd4c, 0x12345678);
        g_assert_cmphex(qtest_readl(qts, FMSS + 0xd34), ==, d34[i]);
        g_assert_cmphex(qtest_readl(qts, FMSS + 0xd48), ==, d48[i]);
        g_assert_cmphex(qtest_readl(qts, FMSS + 0xd4c), ==, 0x12345678);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x2000), ==, d34[i]);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x2004), ==, d48[i] | 0x801);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd38), ==, 0);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd34), ==, 0);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd48), ==, 0);
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void script_set_csgenr15(QTestState *qts, uint32_t value)
{
    const uint32_t program[] = {
        0x05000000, value, 0x02000d54, 0, 0, 0
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
        qtest_writel(qts, RAM + 4 * i, program[i]);
    }
    qtest_writel(qts, FMSS + 0xc04, RAM);
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    qtest_writel(qts, FMSS + 0xc00, 8);
}

static void script_set_accumulator(QTestState *qts, uint32_t value)
{
    const uint32_t program[] = {
        0x05050000, value, 0x02050d3c, 0,
        0x04010d3c, 0xffffffff, 0x05020000, RAM + 0x2400,
        0x11010002, 0, 0, 0
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
        qtest_writel(qts, RAM + 4 * i, program[i]);
    }
    qtest_writel(qts, FMSS + 0xc04, RAM);
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd3c), ==, value);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x2400), ==, value);
    qtest_writel(qts, FMSS + 0xc00, 8);
}

static void sequencer_accumulator(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    const uint32_t values[] = {0, 1, 0x40000000, 0x80000000, 0xffffffff};
    for (unsigned i = 0; i < G_N_ELEMENTS(values); i++) {
        script_set_accumulator(qts, values[i]);
    }
    qtest_writel(qts, RAM, 0x01000d3c);
    qtest_writel(qts, RAM + 4, 0);
    qtest_writel(qts, RAM + 8, 0);
    qtest_writel(qts, RAM + 12, 0);
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd3c), ==, 0);
    qtest_writel(qts, FMSS + 0xc00, 8);
    uint32_t rejected[] = {
        0x05000000, 7, 0x01000d3c, 0xcafebabe,
        0x05020000, RAM + 0x1000, 0x11000002, 0, 0, 0
    };
    for (unsigned form = 0; form < 3; form++) {
        script_set_accumulator(qts, 0x12345678);
        rejected[2] = form == 2 ? 0x01010d3c : (form ? 0x02000d3c : 0x01000d3c);
        rejected[3] = form == 2 ? 0 : (form ? 1 : 0xcafebabe);
        for (unsigned i = 0; i < G_N_ELEMENTS(rejected); i++) {
            qtest_writel(qts, RAM + 4 * i, rejected[i]);
        }
        qtest_writel(qts, RAM + 0x1000, 0xabcddcba);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, FMSS + 0xd3c), ==, 0x12345678);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, 0xabcddcba);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd3c), ==, 0);
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void script_set_scratch(QTestState *qts, uint32_t value)
{
    const uint32_t program[] = {
        0x05040000, value, 0x02040d7c, 0,
        0x04010d7c, 0xffffffff, 0x05020000, RAM + 0x2000,
        0x11010002, 0, 0, 0
    };
    for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
        qtest_writel(qts, RAM + 4 * i, program[i]);
    }
    qtest_writel(qts, FMSS + 0xc04, RAM);
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd7c), ==, value);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x2000), ==, value);
    qtest_writel(qts, FMSS + 0xc00, 8);
}

static void sequencer_scratch(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    const uint32_t values[] = {0, 1, 0x001f0001, 0x80000000, 0xffffffff};
    for (unsigned i = 0; i < G_N_ELEMENTS(values); i++) {
        script_set_scratch(qts, values[i]);
    }
    /* Stock opcode01 D7C initializer takes literal, independent of r[a]. */
    uint32_t immediate[] = {
        0x05000000, 0x12345678, 0x01000d7c, 0,
        0x04010d7c, 0xffffffff, 0x05020000, RAM + 0x2000,
        0x11010002, 0, 0, 0
    };
    const uint32_t literals[] = {0, 0xcafebabe, 0x80000000, 0xffffffff};
    for (unsigned j = 0; j < G_N_ELEMENTS(literals); j++) {
        immediate[3] = literals[j];
        for (unsigned i = 0; i < G_N_ELEMENTS(immediate); i++) {
            qtest_writel(qts, RAM + 4 * i, immediate[i]);
        }
        qtest_writel(qts, FMSS + 0xc04, RAM);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, FMSS + 0xd7c), ==, literals[j]);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x2000), ==, literals[j]);
        qtest_writel(qts, FMSS + 0xc00, 8);
        /* Same latch accepts subsequent measured register assignment. */
        script_set_scratch(qts, ~literals[j]);
    }
    const uint32_t rejected[] = {
        0x05000000, 7, 0x02000d7c, 1,
        0x05020000, RAM + 0x1000, 0x11000002, 0, 0, 0
    };
    script_set_scratch(qts, 0x12345678);
    for (unsigned i = 0; i < G_N_ELEMENTS(rejected); i++) {
        qtest_writel(qts, RAM + 4 * i, rejected[i]);
    }
    qtest_writel(qts, RAM + 0x1000, 0xabcddcba);
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd7c), ==, 0x12345678);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, 0xabcddcba);
    qtest_writel(qts, FMSS + 0xc00, 8);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd7c), ==, 0);
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void chunk_counter(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    const uint32_t program[] = {
        0x04000d28, 0xffffffff, 0x02000d54, 0,
        0x05010000, 0, 0x05020000, RAM + 0x1000,
        0x04070d54, 0xffffffff, 0x0d070007, 1,
        0x02070d54, 0, 0x0c010001, 1,
        0x0e070000, 0x20,
        0x04000d54, 0xffffffff, 0x11000002, 0,
        0x0c020002, 4, 0x11010002, 0, 0, 0
    };
    static const unsigned counts[] = {1, 2, 3, 7};
    for (unsigned j = 0; j < G_N_ELEMENTS(counts); j++) {
        script_set_csgenr15(qts, 0xdeadbeef);
        qtest_writel(qts, FMSS + 0xd28, counts[j]);
        for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
            qtest_writel(qts, RAM + 4 * i, program[i]);
        }
        qtest_writel(qts, RAM + 0x1000, 0xdeadbeef);
        qtest_writel(qts, RAM + 0x1004, 0xdeadbeef);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, 0);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1004), ==, counts[j]);
        g_assert_cmphex(qtest_readl(qts, FMSS + 0xd54), ==, 0);
        g_assert_cmphex(qtest_readl(qts, FMSS + 0xd28), ==, counts[j]);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    script_set_csgenr15(qts, 0);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd54), ==, 0);
    uint32_t rejected[] = {
        0x05000000, 7, 0x01000d54, 3,
        0x05020000, RAM + 0x1000, 0x11000002, 0, 0, 0
    };
    for (unsigned form = 0; form < 2; form++) {
        script_set_csgenr15(qts, 0x12345678);
        rejected[2] = form ? 0x02000d54 : 0x01000d54;
        for (unsigned i = 0; i < G_N_ELEMENTS(rejected); i++) {
            qtest_writel(qts, RAM + 4 * i, rejected[i]);
        }
        qtest_writel(qts, RAM + 0x1000, 0xabcddcba);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, FMSS + 0xd54), ==, 0x12345678);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, 0xabcddcba);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void chunk_counter_snapshot(void)
{
    char *overlay;
    g_autofree char *state = NULL;
    QTestState *qts = start_board(&overlay);
    script_set_accumulator(qts, 0x40000020);
    script_set_scratch(qts, 0x80010001);
    script_set_csgenr15(qts, 0x89abcdef);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd3c), ==, 0x40000020);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd7c), ==, 0x80010001);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd54), ==, 0x89abcdef);
    g_assert_true(migrate(qts, &state));
    qtest_quit(qts);
    qts = qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s,nandrw=%s "
                      "-display none -audio driver=none -nic none -d unimp "
                      "-incoming defer", rom_path, nor_path, nand_path, overlay);
    script_set_accumulator(qts, 0xffffffff);
    script_set_scratch(qts, 0xffffffff);
    script_set_csgenr15(qts, 0x76543210);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd54), ==, 0x76543210);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    qtest_qmp_assert_success(qts, "{ 'execute': 'migrate-incoming', "
                                "'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(qts, "RESUME");
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd3c), ==, 0x40000020);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd7c), ==, 0x80010001);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd54), ==, 0x89abcdef);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd54), ==, 0);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd7c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd3c), ==, 0);
    qtest_quit(qts);
    unlink(state);
    rmdir(overlay);
    g_free(overlay);
}

static void d38_parameter(void)
{
    char *overlay = NULL;
    QTestState *qts = start_board(&overlay);
    const uint32_t values[] = { 0, 12, 25, 0x80000000, 0xffffffff };
    const uint32_t script[] = {
        0x04000d38, 0xffffffff, 0x02000030, 0,
        0x04010030, 0xffffffff, 0x05020000, RAM + 0x2000,
        0x11010002, 0, 0, 0
    };
    /* The observed D38-to-FMDNUM pair followed by shadow observation. */
    qtest_writel(qts, FMSS + 0xd30, 0);
    for (unsigned i = 0; i < G_N_ELEMENTS(script); i++) {
        qtest_writel(qts, RAM + 4 * i, script[i]);
    }
    qtest_writel(qts, FMSS + 0xc04, RAM);
    for (unsigned i = 0; i < G_N_ELEMENTS(values); i++) {
        qtest_writel(qts, FMSS + 0xd38, values[i]);
        g_assert_cmphex(qtest_readl(qts, FMSS + 0xd38), ==, values[i]);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x2000), ==, values[i]);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    g_assert_cmphex(qtest_readl(qts, FMSS + 0xd38), ==, 0);
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void register_or(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    uint32_t program[] = {
        0x05000000, 0x801, 0x04010d4c, 0xffffffff,
        0x0b000001, 0, 0x02000000, 0,
        0x04020000, 0xffffffff, 0x05070000, RAM + 0x1000,
        0x11020007, 0, 0x05020000, 0xdeadbeee,
        0x0b020001, 0x01000801, 0x0c070007, 4,
        0x11020007, 0, 0x0b010001, 0,
        0x0c070007, 4, 0x11010007, 0, 0, 0
    };
    static const uint32_t inputs[][2] = {
        {0x801, 0}, {0x801, 0x20011000}, {0xff00ff00, 0x0ff00ff0},
        {0, 0x89abcdef}, {0xffffffff, 0}, {0x80000000, 1}
    };
    for (unsigned j = 0; j < G_N_ELEMENTS(inputs); j++) {
        program[1] = inputs[j][0];
        qtest_writel(qts, FMSS + 0xd4c, inputs[j][1]);
        for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
            qtest_writel(qts, RAM + 4 * i, program[i]);
        }
        for (unsigned i = 0; i < 3; i++) {
            qtest_writel(qts, RAM + 0x1000 + 4 * i, 0xdeadbeef);
        }
        qtest_writel(qts, FMSS + 0xc04, RAM);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==,
                        inputs[j][0] | inputs[j][1]);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1004), ==,
                        inputs[j][1] | 0x01000801);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1008), ==, inputs[j][1]);
        g_assert_cmphex(qtest_readl(qts, FMSS + 0xd4c), ==, inputs[j][1]);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void register_left_shift(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    uint32_t program[] = {
        0x05020000, 0, 0x05030000, 1,
        0x13030002, 0, 0x05070000, RAM + 0x1000,
        0x11030007, 0, 0x0c070007, 4, 0x11020007, 0,
        0x13000003, 1, 0x02000000, 0,
        0x04010000, 0xffffffff, 0x0c070007, 4,
        0x11010007, 0, 0, 0
    };
    static const uint32_t values[][2] = {
        {0x801, 0}, {0x80000001, 0}, {0x80000001, 1},
        {3, 1}, {1, 31}, {0x12345678, 4}, {0, 31},
        {1, 0}, {1, 1}, {1, 2}, {1, 3}
    };
    for (unsigned j = 0; j < G_N_ELEMENTS(values); j++) {
        program[1] = values[j][1];
        program[3] = values[j][0];
        for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
            qtest_writel(qts, RAM + 4 * i, program[i]);
        }
        for (unsigned i = 0; i < 3; i++) {
            qtest_writel(qts, RAM + 0x1000 + 4 * i, 0xdeadbeef);
        }
        qtest_writel(qts, FMSS + 0xc04, RAM);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        uint32_t shifted = values[j][0] << values[j][1];
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, shifted);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1004), ==, values[j][1]);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1008), ==, shifted << 1);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    /* Preserve nonzero-immediate source-register behavior with distinct dest. */
    program[1] = 3; program[3] = 1; program[5] = 1;
    for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
        qtest_writel(qts, RAM + 4 * i, program[i]);
    }
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, 6);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x1004), ==, 3);
    qtest_writel(qts, FMSS + 0xc00, 8);
    static const uint32_t counts[] = {32, 33, 0xffffffff};
    program[5] = 0;
    for (unsigned j = 0; j < G_N_ELEMENTS(counts); j++) {
        program[1] = counts[j];
        for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
            qtest_writel(qts, RAM + 4 * i, program[i]);
        }
        qtest_writel(qts, RAM + 0x1000, 0xabcddcba);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, 0xabcddcba);
        qtest_writel(qts, FMSS + 0xc00, 8);
    }
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

/* Sequencer transactions use QEMU physical memory results and LE wire words. */
static QTestState *start_transaction_board(char **overlay, char **log)
{
    *overlay = g_dir_make_tmp("fmss-transactions-XXXXXX", NULL);
    *log = g_build_filename(*overlay, "transactions.log", NULL);
    return qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s,nandrw=%s "
                       "-display none -audio driver=none -nic none "
                       "-d guest_errors -D %s", rom_path, nor_path, nand_path,
                       *overlay, *log);
}

static void put_transaction_program(QTestState *qts, const uint32_t *words,
                                    unsigned count)
{
    /* Write the protocol bytes explicitly, independent of host byte order. */
    for (unsigned i = 0; i < count; i++) {
        for (unsigned byte = 0; byte < 4; byte++) {
            qtest_writeb(qts, RAM + 4 * i + byte, words[i] >> (8 * byte));
        }
    }
    qtest_writel(qts, FMSS + 0xc04, RAM);
}

static void sequencer_transactions(void)
{
    char *overlay, *log;
    QTestState *qts = start_transaction_board(&overlay, &log);
    const uint32_t normal[] = {
        0x05000000, 0x67452301, 0x05010000, RAM + 0x1000,
        0x11000001, 0, 0, 0
    };
    put_transaction_program(qts, normal, G_N_ELEMENTS(normal));
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    const uint8_t expected[] = {1, 0x23, 0x45, 0x67};
    for (unsigned i = 0; i < sizeof(expected); i++) {
        g_assert_cmphex(qtest_readb(qts, RAM + 0x1000 + i), ==, expected[i]);
    }
    qtest_writel(qts, FMSS + 0xc00, 8);
    const uint32_t failed_store[] = {
        0x05000000, 0x12345678, 0x05010000, 0xfffffff0,
        0x11000001, 0, 0x05010000, RAM + 0x1000,
        0x11000001, 0, 0, 0
    };
    put_transaction_program(qts, failed_store, G_N_ELEMENTS(failed_store));
    qtest_writel(qts, RAM + 0x1000, 0xabcddcba);
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, 0xabcddcba);
    qtest_writel(qts, FMSS + 0xc00, 8);
    /* Keep an earlier successful store, then jump into unmapped physical
     * memory. A failed fetch must be reported rather than decoded as END. */
    const uint32_t failed_fetch[] = {
        0x05000000, 1, 0x05010000, RAM + 0x1000,
        0x11000001, 0, 0x0e000000, (uint32_t)(0xf0000000ULL - RAM)
    };
    put_transaction_program(qts, failed_fetch, G_N_ELEMENTS(failed_fetch));
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    g_assert_cmphex(qtest_readl(qts, RAM + 0x1000), ==, 1);
    qtest_writel(qts, FMSS + 0xc00, 8);
    qtest_writel(qts, FMSS + 0xc04, 0xfffffff0);
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    qtest_writel(qts, FMSS + 0xc00, 8);
    /* No assertion about abort IRQ or absence of completion: the unchanged
     * CPU C00 handler still schedules its timer after any interpreter return. */
    qtest_quit(qts);
    g_autofree char *text = NULL;
    g_assert_true(g_file_get_contents(log, &text, NULL, NULL));
    g_assert_nonnull(strstr(text, "sequencer store at 0xfffffff0 failed"));
    g_assert_nonnull(strstr(text, "instruction fetch at 0xf0000000 failed"));
    g_assert_nonnull(strstr(text, "instruction fetch at 0xfffffff0 failed"));
    unlink(log);
    rmdir(overlay);
    g_free(log);
    g_free(overlay);
}

#define CPU_DMA_UNMAPPED 0xf0000000ULL
#define CPU_DMA_RAM_END (0x0ff00000ULL + 0x00100000ULL)

/* Board mapping: secure RAM ends0x0fb04000, but framebuffer0x0fb00000
 * spans4MiB to0x0ff00000 and iBoot RAM then spans1MiB to0x10000000.
 * The first actual unmapped boundary of this contiguous range is0x10000000. */
static void expect_cpu_bytes(QTestState *qts, uint64_t address, size_t count,
                             uint8_t expected)
{
    uint8_t bytes[4096];
    g_assert_cmpuint(count, <=, sizeof(bytes));
    qtest_memread(qts, address, bytes, count);
    for (size_t i = 0; i < count; i++) {
        g_assert_cmphex(bytes[i], ==, expected);
    }
}

static void cpu_read_setup(QTestState *qts)
{
    /* Two distinct pages, two halves each, two twelve-byte metadata records. */
    qtest_writel(qts, RAM + 0x8000, 37);
    qtest_writel(qts, RAM + 0x8004, 38);
    qtest_writel(qts, RAM + 0x8100, 1);
    qtest_writel(qts, RAM + 0x8104, 1);
    for (unsigned i = 0; i < 4; i++) {
        qtest_writel(qts, RAM + 0x8200 + 4 * i, RAM + 0x40000 + 0x800 * i);
    }
    qtest_memset(qts, RAM + 0x40000, 0xcc, 8192);
    qtest_memset(qts, RAM + 0x8300, 0xcc, 64);
    qtest_writel(qts, FMSS + 0xd0c, RAM + 0x8000);
    qtest_writel(qts, FMSS + 0xd10, RAM + 0x8100);
    qtest_writel(qts, FMSS + 0xd18, 2);
    qtest_writel(qts, FMSS + 0xd20, RAM + 0x8200);
    qtest_writel(qts, FMSS + 0xd1c, RAM + 0x8300);
    qtest_writel(qts, FMSS + 0xd30, 0xa01);
}

static void cpu_read_transactions(void)
{
    char *saved_nand = nand_path;
    g_autofree char *base = g_dir_make_tmp("fmss-cpu-dma-base-XXXXXX", NULL);
    g_autofree char *chip = g_build_filename(base, "cs0", NULL);
    g_autofree char *first = g_build_filename(chip, "37.page", NULL);
    g_autofree char *second = g_build_filename(chip, "38.page", NULL);
    uint8_t record[4160];
    g_assert_cmpint(g_mkdir_with_parents(chip, 0755), ==, 0);
    memset(record, 0xa3, 4096); memset(record + 4096, 0xb4, 64);
    g_assert_true(g_file_set_contents(first, (char *)record, sizeof(record), NULL));
    memset(record, 0xc5, 4096); memset(record + 4096, 0xd6, 64);
    g_assert_true(g_file_set_contents(second, (char *)record, sizeof(record), NULL));
    nand_path = base;
    for (unsigned mode = 0; mode < 6; mode++) {
        char *overlay, *log;
        QTestState *qts = start_transaction_board(&overlay, &log);
        cpu_read_setup(qts);
        switch (mode) {
        case 1: /* Failed first page descriptor: no NAND-derived guest writes. */
            qtest_writel(qts, FMSS + 0xd0c, CPU_DMA_UNMAPPED);
            break;
        case 2: /* First output pointer valid, second descriptor unmapped. */
            qtest_writel(qts, CPU_DMA_RAM_END - 4, RAM + 0x40000);
            qtest_writel(qts, FMSS + 0xd20, CPU_DMA_RAM_END - 4);
            break;
        case 3: /* Destination failure after a completed first half. */
            qtest_writel(qts, RAM + 0x8204, CPU_DMA_UNMAPPED);
            break;
        case 4: /* One whole page completed before second page descriptor fails. */
            qtest_writel(qts, CPU_DMA_RAM_END - 4, 37);
            qtest_writel(qts, FMSS + 0xd0c, CPU_DMA_RAM_END - 4);
            break;
        case 5: /* One AddressSpace write crosses RAM/unmapped: prefix remains. */
            qtest_memset(qts, CPU_DMA_RAM_END - 1024, 0xcc, 1024);
            qtest_writel(qts, RAM + 0x8200, CPU_DMA_RAM_END - 1024);
            break;
        }
        qtest_writel(qts, FMSS + 0xd38, 1);
        if (mode == 0) {
            expect_cpu_bytes(qts, RAM + 0x40000, 4096, 0xa3);
            expect_cpu_bytes(qts, RAM + 0x41000, 4096, 0xc5);
            expect_cpu_bytes(qts, RAM + 0x8300, 12, 0xb4);
            expect_cpu_bytes(qts, RAM + 0x830c, 12, 0xd6);
            expect_cpu_bytes(qts, RAM + 0x8318, 40, 0xcc);
        } else {
            expect_cpu_bytes(qts, RAM + 0x41000, 4096, 0xcc);
            expect_cpu_bytes(qts, RAM + 0x40000,
                             mode == 4 ? 4096 : 2048,
                             mode >= 2 && mode <= 4 ? 0xa3 : 0xcc);
            if (mode != 4) {
                expect_cpu_bytes(qts, RAM + 0x40800, 2048, 0xcc);
            }
            expect_cpu_bytes(qts, RAM + 0x8300, 12, mode == 4 ? 0xb4 : 0xcc);
            expect_cpu_bytes(qts, RAM + 0x830c, 52, 0xcc);
            if (mode == 5) {
                expect_cpu_bytes(qts, CPU_DMA_RAM_END - 1024, 1024, 0xa3);
            }
        }
        qtest_quit(qts);
        if (mode != 0) {
            g_autofree char *text = NULL;
            g_assert_true(g_file_get_contents(log, &text, NULL, NULL));
            g_assert_nonnull(strstr(text, "failed; transfer stopped"));
        }
        /* No abort/completion/IRQ assertion: those contracts are unchanged. */
        unlink(log); rmdir(overlay); g_free(log); g_free(overlay);
    }
    nand_path = saved_nand;
    unlink(first); unlink(second); rmdir(chip); rmdir(base);
}

static void cpu_write_source_guard(void)
{
    char *overlay, *log;
    QTestState *qts = start_transaction_board(&overlay, &log);
    qtest_writel(qts, RAM, 1);
    qtest_writel(qts, RAM + 4, 39);
    qtest_writel(qts, RAM + 8, 0);
    qtest_writel(qts, RAM + 0x1000, CPU_DMA_UNMAPPED);
    qtest_writel(qts, RAM + 0x1004, RAM + 0x10800);
    qtest_writel(qts, FMSS + 0xd10, RAM);
    qtest_writel(qts, FMSS + 0xd20, RAM + 0x1000);
    qtest_writel(qts, FMSS + 0xd1c, RAM + 0x2000);
    qtest_writel(qts, FMSS + 0xd30, 0xa02);
    qtest_writel(qts, FMSS + 0xd38, 1);
    QDict *reply = qtest_qmp(qts, "{ 'execute': 'query-status' }");
    QDict *status = qdict_get_qdict(reply, "return");
    g_assert_cmpstr(qdict_get_str(status, "status"), ==, "io-error");
    qobject_unref(reply);
    qtest_quit(qts);
    g_autofree char *page = g_build_filename(overlay, "cs0", "39.page", NULL);
    g_assert_false(g_file_test(page, G_FILE_TEST_EXISTS));
    /* Existing error_report diagnostic goes to stderr, not the -D log. */
    /* This is the existing RAM-only source/fatal policy, not a new IRQ claim. */
    unlink(log); rmdir(overlay); g_free(log); g_free(overlay);
}

/* Observe the existing latch through the sequencer, not unmodeled CPU D10
 * readback. This helper performs no NAND command or descriptor DMA. */
static uint32_t script_observe_d10(QTestState *qts)
{
    const uint32_t observe[] = {
        0x04000d10, 0xffffffff, 0x05070000, RAM + 0x2000,
        0x11000007, 0, 0, 0
    };
    put_transaction_program(qts, observe, G_N_ELEMENTS(observe));
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    uint32_t value = qtest_readl(qts, RAM + 0x2000);
    qtest_writel(qts, FMSS + 0xc00, 8);
    return value;
}

static void descriptor_pointer_backstep(void)
{
    char *overlay;
    QTestState *qts = start_board(&overlay);
    const uint32_t desc = RAM + 0x3000, output = RAM + 0x2000;
    const uint32_t program[] = {
        0x04000d10, 0xffffffff, 0x03010000, 0,
        0x0c000000, 4, 0x02000d10, 0,
        0x04000d10, 0xffffffff, 0x03020000, 0,
        0x0d000000, 4, 0x02000d10, 0,
        0x04000d10, 0xffffffff, 0x03030000, 0,
        0x05070000, output, 0x11010007, 0,
        0x0c070007, 4, 0x11020007, 0,
        0x0c070007, 4, 0x11030007, 0,
        0x0c070007, 4, 0x11000007, 0, 0, 0
    };
    qtest_writel(qts, desc, 0x11112222);
    qtest_writel(qts, desc + 4, 0x33334444);
    /* CPU latch setup precedes script; no D38 shortcut mutation. */
    qtest_writel(qts, FMSS + 0xd10, desc);
    for (unsigned i = 0; i < G_N_ELEMENTS(program); i++) {
        qtest_writel(qts, RAM + 4*i, program[i]);
    }
    qtest_writel(qts, FMSS + 0xc04, RAM);
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    g_assert_cmphex(qtest_readl(qts, output), ==, 0x11112222);
    g_assert_cmphex(qtest_readl(qts, output + 4), ==, 0x33334444);
    g_assert_cmphex(qtest_readl(qts, output + 8), ==, 0x11112222);
    g_assert_cmphex(qtest_readl(qts, output + 12), ==, desc);
    qtest_writel(qts, FMSS + 0xc00, 8);
    for (unsigned form = 0; form < 2; form++) {
        const uint32_t rejected[] = {
            0x05000000, desc + 4,
            form ? 0x01000d10 : 0x02000d10, form ? desc + 4 : 1,
            0x04000d10, 0xffffffff, 0x03010000, 0,
            0x05070000, output, 0x11010007, 0, 0, 0
        };
        put_transaction_program(qts, rejected, G_N_ELEMENTS(rejected));
        qtest_writel(qts, output, 0xabcddcba);
        qtest_writel(qts, FMSS + 0xc00, 0xffb5);
        g_assert_cmphex(qtest_readl(qts, output), ==, 0xabcddcba);
        qtest_writel(qts, FMSS + 0xc00, 8);
        g_assert_cmphex(script_observe_d10(qts), ==, desc);
    }
    qtest_quit(qts);
    rmdir(overlay);
    g_free(overlay);
}

static void descriptor_pointer_snapshot(void)
{
    char *overlay;
    g_autofree char *state = NULL;
    QTestState *qts = start_board(&overlay);
    const uint32_t desc = RAM + 0x3000;
    const uint32_t advance[] = {
        0x04000d10, 0xffffffff, 0x0c000000, 4,
        0x02000d10, 0, 0, 0
    };
    qtest_writel(qts, FMSS + 0xd10, desc);
    put_transaction_program(qts, advance, G_N_ELEMENTS(advance));
    qtest_writel(qts, FMSS + 0xc00, 0xffb5);
    qtest_writel(qts, FMSS + 0xc00, 8);
    g_assert_cmphex(script_observe_d10(qts), ==, desc + 4);
    g_assert_true(migrate(qts, &state));
    qtest_quit(qts);
    qts = qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s,nandrw=%s "
                      "-display none -audio driver=none -nic none -d unimp "
                      "-incoming defer", rom_path, nor_path, nand_path, overlay);
    qtest_writel(qts, FMSS + 0xd10, desc + 12);
    g_assert_cmphex(script_observe_d10(qts), ==, desc + 12);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    qtest_qmp_assert_success(qts, "{ 'execute': 'migrate-incoming', "
                                "'arguments': { 'uri': %s } }", uri);
    qtest_qmp_eventwait(qts, "RESUME");
    g_assert_cmphex(script_observe_d10(qts), ==, desc + 4);
    qtest_qmp_assert_success(qts, "{ 'execute': 'system_reset' }");
    g_assert_cmphex(script_observe_d10(qts), ==, 0);
    qtest_quit(qts);
    unlink(state); rmdir(overlay); g_free(overlay);
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
    qtest_add_func("/ipod/fmss/descriptor-pointer-backstep", descriptor_pointer_backstep);
    qtest_add_func("/ipod/fmss/descriptor-pointer-snapshot", descriptor_pointer_snapshot);
    qtest_add_func("/ipod/fmss/logical-and", logical_and);
    qtest_add_func("/ipod/fmss/nonabsence-open-fallback", nonabsence_open_fallback);
    qtest_add_func("/ipod/fmss/directory-physical-erased", directory_physical);
    qtest_add_func("/ipod/fmss/directory-generated-blank", directory_generated);
    qtest_add_func("/ipod/fmss/packed-physical-erased", packed_physical);
    qtest_add_func("/ipod/fmss/packed-generated-blank", packed_generated);
    qtest_add_func("/ipod/fmss/read-id-chip-selection", read_id_chip_selection);
    qtest_add_func("/ipod/fmss/observed-right-shift", observed_right_shift);
    qtest_add_func("/ipod/fmss/parameter-latches", parameter_latches);
    qtest_add_func("/ipod/fmss/d38-parameter", d38_parameter);
    qtest_add_func("/ipod/fmss/sequencer-accumulator", sequencer_accumulator);
    qtest_add_func("/ipod/fmss/sequencer-scratch", sequencer_scratch);
    qtest_add_func("/ipod/fmss/chunk-counter", chunk_counter);
    qtest_add_func("/ipod/fmss/chunk-counter-snapshot", chunk_counter_snapshot);
    qtest_add_func("/ipod/fmss/register-or", register_or);
    qtest_add_func("/ipod/fmss/register-left-shift", register_left_shift);
    qtest_add_func("/ipod/fmss/sequencer-transactions", sequencer_transactions);
    qtest_add_func("/ipod/fmss/cpu-read-transactions", cpu_read_transactions);
    qtest_add_func("/ipod/fmss/cpu-write-source-guard", cpu_write_source_guard);
    result = g_test_run();
    unlink(rom_path); unlink(nor_path); rmdir(nand_path);
    g_free(rom_path); g_free(nor_path); g_free(nand_path);
    return result;
}
