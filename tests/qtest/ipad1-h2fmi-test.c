/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Blank raw flash, no FTL seed and no guest instructions. Reproduce the stock
 * IOP's normal-read -> raw-read register sequence seen during initial BBT scan. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define FMI 0x81200000ULL
#define FMC (FMI + 0x40000)
#define PAGE_BYTES 4096
#define SPARE_BYTES 128
static char *rom_path, *nand_path;

static void page_command(QTestState *qts, unsigned page)
{
    qtest_writel(qts, FMC + 0x0c, 1);
    qtest_writel(qts, FMC + 0x14, 0x3000);
    qtest_writel(qts, FMC + 0x18, page << 16);
    qtest_writel(qts, FMC + 0x1c, 0);
    qtest_writel(qts, FMC + 0x10, 0xb);
}

static void read_raw_after_normal(void)
{
    QTestState *qts = qtest_initf("-machine ipad1,bootrom=%s,nand=%s "
                                "-display none -audio driver=none -nic none",
                                rom_path, nand_path);
    qtest_writel(qts, FMI + 0x34, 0x14540004);
    page_command(qts, 2);
    qtest_writel(qts, FMI + 4, 3);
    for (unsigned i = 0; i < PAGE_BYTES / 4; i++) {
        g_assert_cmphex(qtest_readl(qts, FMI + 0x14), ==, 0xffffffff);
    }
    for (unsigned i = 0; i < 10; i++) {
        g_assert_cmphex(qtest_readb(qts, FMI + 0x18), ==, 0xff);
    }
    g_assert_cmphex(qtest_readl(qts, FMI + 0x1c), ==, 0);
    /* Raw read: new page, no reset/idle control write, no bit-7 edge. */
    page_command(qts, 125);
    qtest_writel(qts, FMI + 0x34, 0x40004);
    qtest_writel(qts, FMI + 4, 3);
    for (unsigned i = 0; i < PAGE_BYTES / 4; i++) {
        g_assert_cmphex(qtest_readl(qts, FMI + 0x14), ==, 0xffffffff);
    }
    g_assert_cmphex(qtest_readl(qts, FMI + 0x1c), ==, 0x18);
    g_assert_cmphex(qtest_readl(qts, FMI + 0x0c) & 2, ==, 2);
    qtest_writel(qts, FMI + 0x0c, 2);
    g_assert_cmphex(qtest_readl(qts, FMI + 0x0c) & 2, ==, 0);
    qtest_writel(qts, FMI + 0x34, 0x8001);
    qtest_writel(qts, FMI + 4, 3);
    g_assert_cmphex(qtest_readl(qts, FMI + 0x0c) & 2, ==, 2);
    /* Raw mode streams physical spare bytes through DATA, without ECC/meta
     * extraction. The first version of the model stalled here after data. */
    for (unsigned i = 0; i < SPARE_BYTES / 4; i++) {
        g_assert_cmphex(qtest_readl(qts, FMI + 0x14), ==, 0xffffffff);
    }
    g_assert_cmphex(qtest_readl(qts, FMI + 0x1c), ==, 0);
    qtest_writel(qts, FMI + 0x0c, 2);
    g_assert_cmphex(qtest_readl(qts, FMI + 0x0c) & 2, ==, 0);
    /* An acknowledgment rewriting control must not refill the same page. */
    qtest_writel(qts, FMI + 4, 3);
    g_assert_cmphex(qtest_readl(qts, FMI + 0x1c), ==, 0);
    g_assert_cmphex(qtest_readl(qts, FMI + 0x0c) & 2, ==, 0);
    qtest_quit(qts);
}

static void program_with_ce(QTestState *qts, unsigned page, uint32_t value, uint32_t ce_mask)
{
    qtest_writel(qts, FMI + 4, 0);
    qtest_writel(qts, FMI + 0x0c, 2);
    qtest_writel(qts, FMI + 0x34, 0);
    qtest_writel(qts, FMC + 0x0c, ce_mask);
    qtest_writel(qts, FMC + 0x14, 0x80);
    qtest_writel(qts, FMC + 0x18, page << 16);
    qtest_writel(qts, FMC + 0x1c, 0);
    qtest_writel(qts, FMC + 0x10, 9);
    qtest_writel(qts, FMI + 4, 5);
    for (unsigned i = 0; i < PAGE_BYTES / 4; i++) {
        qtest_writel(qts, FMI + 0x14, value);
    }
    for (unsigned i = 0; i < 10; i++) {
        qtest_writeb(qts, FMI + 0x18, 0);
    }
    g_assert_cmphex(qtest_readl(qts, FMI + 0x0c) & 2, ==, 2);
    qtest_writel(qts, FMC + 0x14, 0x10);
    qtest_writel(qts, FMC + 0x10, 1);
}

static void program(QTestState *qts, unsigned page, uint32_t value)
{
    program_with_ce(qts, page, value, 1);
}

static void physical_program_erase(void)
{
    QTestState *qts = qtest_initf("-machine ipad1,bootrom=%s,nand=%s "
                                "-display none -audio driver=none -nic none",
                                rom_path, nand_path);
    program(qts, 5, 0);
    g_assert_cmphex(qtest_readl(qts, FMC + 0x48), ==, 0xe0);
    program(qts, 5, 0xffffffff); /* Input ones leave existing zeros unchanged. */
    g_assert_cmphex(qtest_readl(qts, FMC + 0x48), ==, 0xe0);
    qtest_writel(qts, FMC + 0x10, 0x40);
    g_assert_cmphex(qtest_readl(qts, FMC + 0x48), ==, 0xe0);
    program_with_ce(qts, 5, 0, 1u << 7); /* Absent chip maps outside populated buses. */
    g_assert_cmphex(qtest_readl(qts, FMC + 0x48), ==, 0xe1);
    program(qts, 512, 0); /* First row beyond this test chip. */
    g_assert_cmphex(qtest_readl(qts, FMC + 0x48), ==, 0xe1);
    qtest_writel(qts, FMC + 0x10, 0x40);
    g_assert_cmphex(qtest_readl(qts, FMC + 0x48), ==, 0xe1);
    qtest_quit(qts);
    /* Actual zero data survives a cold reopen, including a subsequent no-op program. */
    qts = qtest_initf("-machine ipad1,bootrom=%s,nand=%s "
                      "-display none -audio driver=none -nic none", rom_path, nand_path);
    qtest_writel(qts, FMI + 0x34, 0x40004);
    page_command(qts, 5);
    qtest_writel(qts, FMI + 4, 3);
    for (unsigned i = 0; i < PAGE_BYTES / 4; i++) {
        g_assert_cmphex(qtest_readl(qts, FMI + 0x14), ==, 0);
    }
    qtest_writel(qts, FMI + 4, 0);
    /* Erase sends row only, unlike the two column bytes of read/program. */
    qtest_writel(qts, FMC + 0x18, 5);
    qtest_writel(qts, FMC + 0x1c, 7); /* stale prior address bytes must not leak */
    qtest_writel(qts, FMC + 0x20, 2);
    qtest_writel(qts, FMC + 0x14, 0xd060);
    qtest_writel(qts, FMC + 0x10, 0xb);
    g_assert_cmphex(qtest_readl(qts, FMC + 0x48), ==, 0xe0);
    /* Reset drains the spare phase left from the earlier raw read. */
    qtest_writel(qts, FMC + 0x14, 0xff);
    qtest_writel(qts, FMC + 0x10, 1);
    page_command(qts, 5);
    qtest_writel(qts, FMI + 4, 3);
    for (unsigned i = 0; i < PAGE_BYTES / 4; i++) {
        g_assert_cmphex(qtest_readl(qts, FMI + 0x14), ==, 0xffffffff);
    }
    qtest_quit(qts);
}

static void reset_fifo(QTestState *qts)
{
    qtest_writel(qts, FMI + 4, 0);
    qtest_writel(qts, FMC + 0x14, 0xff);
    qtest_writel(qts, FMC + 0x10, 1);
}

static void assert_page(QTestState *qts, unsigned page, uint32_t value)
{
    reset_fifo(qts);
    qtest_writel(qts, FMI + 0x34, 0x40004);
    page_command(qts, page);
    qtest_writel(qts, FMI + 4, 3);
    for (unsigned i = 0; i < PAGE_BYTES / 4; i++) {
        g_assert_cmphex(qtest_readl(qts, FMI + 0x14), ==, value);
    }
}

static void legacy_compatibility(void)
{
    g_autofree char *geometry = g_build_filename(nand_path, "geometry.json", NULL);
    g_autofree char *saved = NULL;
    g_assert_true(g_file_get_contents(geometry, &saved, NULL, NULL));
    g_autofree char *legacy = g_strdup(saved);
    char *format = strstr(legacy, "nand-xor-ff-v2");
    g_assert_nonnull(format);
    /* Same-length unknown formats are forbidden; legacy declaration is
     * explicit instead of changing bytes under an open controller. */
    g_autofree char *value = g_strdup_printf("%.*slegacy-zero-blank-v1%s",
                         (int)(format - legacy), legacy, format + strlen("nand-xor-ff-v2"));
    g_assert_true(g_file_set_contents(geometry, value, -1, NULL));
    QTestState *qts = qtest_initf("-machine ipad1,bootrom=%s,nand=%s -display none -audio driver=none -nic none",
                                rom_path, nand_path);
    program(qts, 5, 0); /* Legacy zero inference remains deliberately ambiguous. */
    assert_page(qts, 5, 0xffffffff);
    program(qts, 5, 0x01010101);
    assert_page(qts, 5, 0x01010101);
    program(qts, 5, 0xffffffff); /* Preserve existing legacy overwrite behavior. */
    assert_page(qts, 5, 0xffffffff);
    qtest_quit(qts);
    g_assert_true(g_file_set_contents(geometry, saved, -1, NULL));
    /* Reinitialize the private fixture before returning to physical v2. */
    g_autofree char *path = g_strdup_printf("%s/bus0-ce0.pages", nand_path);
    int fd = open(path, O_RDWR);
    g_assert_cmpint(fd, >=, 0);
    uint8_t erased[PAGE_BYTES + SPARE_BYTES] = { 0 };
    g_assert_cmpint(pwrite(fd, erased, sizeof(erased), 5 * sizeof(erased)), ==, sizeof(erased));
    close(fd);
}

static void overlay_cold_reopen(void)
{
    g_autofree char *overlay = g_dir_make_tmp("ipad1-nand-overlay-XXXXXX", NULL);
    g_autofree char *base = g_strdup_printf("%s/bus0-ce0.pages", nand_path);
    uint8_t encoded[PAGE_BYTES + SPARE_BYTES];
    memset(encoded, 0x0f, sizeof(encoded)); /* Physical F0 in immutable base. */
    int fd = open(base, O_RDWR);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pwrite(fd, encoded, sizeof(encoded), 5 * sizeof(encoded)), ==, sizeof(encoded));
    close(fd);
    QTestState *qts = qtest_initf("-machine ipad1,bootrom=%s,nand=%s,nand-overlay=%s -display none -audio driver=none -nic none",
                                rom_path, nand_path, overlay);
    program(qts, 5, 0xffffffff);
    assert_page(qts, 5, 0xf0f0f0f0);
    qtest_quit(qts);
    qts = qtest_initf("-machine ipad1,bootrom=%s,nand=%s,nand-overlay=%s -display none -audio driver=none -nic none",
                      rom_path, nand_path, overlay);
    assert_page(qts, 5, 0xf0f0f0f0);
    qtest_writel(qts, FMI + 4, 0);
    qtest_writel(qts, FMC + 0x18, 5);
    qtest_writel(qts, FMC + 0x14, 0xd060);
    qtest_writel(qts, FMC + 0x10, 0xb);
    assert_page(qts, 5, 0xffffffff);
    qtest_quit(qts);
    fd = open(base, O_RDWR);
    uint8_t check[sizeof(encoded)];
    g_assert_cmpint(pread(fd, check, sizeof(check), 5 * sizeof(check)), ==, sizeof(check));
    g_assert_cmpmem(encoded, sizeof(encoded), check, sizeof(check));
    memset(encoded, 0, sizeof(encoded));
    g_assert_cmpint(pwrite(fd, encoded, sizeof(encoded), 5 * sizeof(encoded)), ==, sizeof(encoded));
    close(fd);
    GDir *dir = g_dir_open(overlay, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        g_autofree char *path = g_build_filename(overlay, name, NULL);
        unlink(path);
    }
    g_dir_close(dir);
    rmdir(overlay);
}

static QTestState *overlay_board(const char *overlay)
{
    return qtest_initf("-machine ipad1,bootrom=%s,nand=%s,nand-overlay=%s "
                       "-display none -audio driver=none -nic none",
                       rom_path, nand_path, overlay);
}

static void snapshot_flush(QTestState *q)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("ipad1-ownership-snapshot-XXXXXX", &path, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_autofree char *uri = g_strdup_printf("file:%s", path);
    qtest_qmp_assert_success(q, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    bool completed = false;
    for (unsigned i = 0; i < 10000; i++) {
        QDict *reply = qtest_qmp(q, "{ 'execute': 'query-migrate' }");
        const char *status = qdict_get_try_str(qdict_get_qdict(reply, "return"), "status");
        completed = status && !strcmp(status, "completed");
        bool failed = status && !strcmp(status, "failed");
        qobject_unref(reply);
        g_assert_false(failed);
        if (completed) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_true(completed);
    unlink(path);
}

static void ownership_crash_reopen(void)
{
    g_autofree char *overlay = g_dir_make_tmp("ipad1-crash-overlay-XXXXXX", NULL);
    g_autofree char *bitmap = g_build_filename(overlay, "bus0-ce0.dirty", NULL);
    pid_t child = fork();
    int status;
    g_assert_cmpint(child, >=, 0);
    if (!child) {
        QTestState *q = overlay_board(overlay);
        program(q, 7, 0);
        assert_page(q, 7, 0);
        /* Deliberately skip every graceful QMP/VM stop/cleanup path. The driver
         * reaps its own killed guest, then exits without libqtest's normal
         * status check (a SIGKILL is intentional in this crash test). */
        pid_t guest = qtest_pid(q);
        if (kill(guest, SIGKILL) || waitpid(guest, &status, 0) != guest ||
            !WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL) {
            _exit(1);
        }
        _exit(0);
    }
    g_assert_cmpint(waitpid(child, &status, 0), ==, child);
    g_assert_true(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    for (unsigned i = 0; i < 2; i++) {
        g_autofree char *socket = g_strdup_printf("%s/qtest-%d.%s", g_get_tmp_dir(),
                                                 (int)child, i ? "qmp" : "sock");
        unlink(socket);
    }
    uint8_t bits[64];
    int fd = open(bitmap, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(read(fd, bits, sizeof(bits)), ==, sizeof(bits));
    close(fd);
    g_assert_cmphex(bits[0] & (1u << 7), ==, 0);
    QTestState *q = overlay_board(overlay);
    assert_page(q, 7, 0xffffffff); /* unpublished page cannot hide the base */
    program(q, 7, 0);
    snapshot_flush(q); /* pre-save publishes data -> ownership even when stopped */
    fd = open(bitmap, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(read(fd, bits, sizeof(bits)), ==, sizeof(bits));
    close(fd);
    g_assert_cmphex(bits[0] & (1u << 7), ==, 1u << 7);
    qtest_quit(q);
    q = overlay_board(overlay);
    assert_page(q, 7, 0);
    qtest_quit(q);
    GDir *dir = g_dir_open(overlay, 0, NULL);
    const char *name;
    while ((name = g_dir_read_name(dir))) {
        g_autofree char *path = g_build_filename(overlay, name, NULL);
        unlink(path);
    }
    g_dir_close(dir);
    rmdir(overlay);
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

static void second_writer_refused(void)
{
    QTestState *qts = qtest_initf("-machine ipad1,bootrom=%s,nand=%s -display none -audio driver=none -nic none",
                                rom_path, nand_path);
    g_autofree char *machine = g_strdup_printf("ipad1,bootrom=%s,nand=%s", rom_path, nand_path);
    char *argv[] = { getenv("QTEST_QEMU_BINARY"), "-machine", machine, "-S",
                    "-display", "none", "-audio", "driver=none", "-monitor", "none", "-serial", "none", NULL };
    expect_startup_failure(argv, "lock");
    qtest_quit(qts);
}

static void snapshot_format_mismatch(void)
{
    g_autofree char *snapshot = NULL;
    int fd = g_file_open_tmp("ipad1-nand-state-XXXXXX", &snapshot, NULL);
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_autofree char *uri = g_strdup_printf("file:%s", snapshot);
    QTestState *qts = qtest_initf("-machine ipad1,bootrom=%s,nand=%s -display none -audio driver=none -nic none",
                                rom_path, nand_path);
    program(qts, 8, 0x5a5a5a5a);
    page_command(qts, 8);
    qtest_writel(qts, FMI + 4, 3); /* nonempty fixed-array FIFOs in VM stream */
    qtest_qmp_assert_success(qts, "{ 'execute': 'migrate', 'arguments': { 'uri': %s } }", uri);
    bool completed = false;
    for (unsigned i = 0; i < 10000; i++) {
        QDict *reply = qtest_qmp(qts, "{ 'execute': 'query-migrate' }");
        const char *status = qdict_get_try_str(qdict_get_qdict(reply, "return"), "status");
        completed = status && !strcmp(status, "completed");
        bool failed = status && !strcmp(status, "failed");
        qobject_unref(reply);
        g_assert_false(failed);
        if (completed) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_true(completed);
    qtest_quit(qts);
    g_autofree char *geometry = g_build_filename(nand_path, "geometry.json", NULL);
    g_autofree char *saved = NULL;
    g_assert_true(g_file_get_contents(geometry, &saved, NULL, NULL));
    const char *format = strstr(saved, "nand-xor-ff-v2");
    g_assert_nonnull(format);
    g_autofree char *legacy = g_strdup_printf("%.*slegacy-zero-blank-v1%s",
                        (int)(format - saved), saved, format + strlen("nand-xor-ff-v2"));
    g_assert_true(g_file_set_contents(geometry, legacy, -1, NULL));
    g_autofree char *machine = g_strdup_printf("ipad1,bootrom=%s,nand=%s", rom_path, nand_path);
    char *argv[] = { getenv("QTEST_QEMU_BINARY"), "-machine", machine, "-accel", "qtest",
                    "-incoming", uri, "-S", "-display", "none", "-audio", "driver=none",
                    "-monitor", "none", "-serial", "none", NULL };
    expect_startup_failure(argv, "snapshot NAND storage format mismatch");
    g_assert_true(g_file_set_contents(geometry, saved, -1, NULL));
    qts = qtest_initf("-machine ipad1,bootrom=%s,nand=%s -incoming %s -display none -audio driver=none -nic none",
                      rom_path, nand_path, uri);
    completed = false;
    for (unsigned i = 0; i < 10000; i++) {
        QDict *reply = qtest_qmp(qts, "{ 'execute': 'query-migrate' }");
        const char *status = qdict_get_try_str(qdict_get_qdict(reply, "return"), "status");
        completed = status && !strcmp(status, "completed");
        bool failed = status && !strcmp(status, "failed");
        qobject_unref(reply);
        g_assert_false(failed);
        if (completed) {
            break;
        }
        g_usleep(1000);
    }
    g_assert_true(completed);
    /* Matching incoming state must reactivate actual backend I/O. */
    g_assert_cmphex(qtest_readl(qts, FMI + 0x14), ==, 0x5a5a5a5a);
    g_assert_cmphex(qtest_readb(qts, FMI + 0x18), ==, 0);
    assert_page(qts, 7, 0xffffffff);
    program(qts, 7, 0x55555555);
    assert_page(qts, 7, 0x55555555);
    qtest_quit(qts);
    unlink(snapshot);
}

int main(int argc, char **argv)
{
    g_autofree char *rom = g_malloc0(65536);
    g_autofree char *geometry = NULL;
    int fd = g_file_open_tmp("ipad1-h2fmi-rom-XXXXXX", &rom_path, NULL);
    int result;
    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(rom_path, rom, 65536, NULL));
    nand_path = g_dir_make_tmp("ipad1-h2fmi-nand-XXXXXX", NULL);
    g_assert_nonnull(nand_path);
    geometry = g_build_filename(nand_path, "geometry.json", NULL);
    g_assert_true(g_file_set_contents(geometry,
        "{\"storage_format\":\"nand-xor-ff-v2\",\"page_bytes\":4096,\"spare_bytes\":128,\"pages_per_block\":128,"
        "\"blocks_per_ce\":4,\"ce_per_bus\":4,\"buses\":2,\"chip_id\":\"0xB614D5AD\"}", -1, NULL));
    for (unsigned bus = 0; bus < 2; bus++) {
        for (unsigned ce = 0; ce < 4; ce++) {
            g_autofree char *path = g_strdup_printf("%s/bus%u-ce%u.pages", nand_path, bus, ce);
            fd = open(path, O_CREAT | O_RDWR, 0600);
            g_assert_cmpint(fd, >=, 0);
            g_assert_cmpint(ftruncate(fd, (PAGE_BYTES + SPARE_BYTES) * 128 * 4), ==, 0);
            close(fd);
        }
    }
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipad1/h2fmi/raw-after-normal", read_raw_after_normal);
    qtest_add_func("/ipad1/h2fmi/physical-program-erase", physical_program_erase);
    qtest_add_func("/ipad1/h2fmi/legacy-compatibility", legacy_compatibility);
    qtest_add_func("/ipad1/h2fmi/overlay-cold-reopen", overlay_cold_reopen);
    qtest_add_func("/ipad1/h2fmi/ownership-crash-reopen", ownership_crash_reopen);
    qtest_add_func("/ipad1/h2fmi/second-writer-refused", second_writer_refused);
    qtest_add_func("/ipad1/h2fmi/snapshot-format-mismatch", snapshot_format_mismatch);
    result = g_test_run();
    for (unsigned bus = 0; bus < 2; bus++) {
        for (unsigned ce = 0; ce < 4; ce++) {
            g_autofree char *path = g_strdup_printf("%s/bus%u-ce%u.pages", nand_path, bus, ce);
            unlink(path);
        }
    }
    unlink(geometry);
    rmdir(nand_path);
    unlink(rom_path);
    g_free(nand_path);
    g_free(rom_path);
    return result;
}
