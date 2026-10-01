/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Compatibility handoff data on the production N72 board, without guest code. */
#include "qemu/osdep.h"
#include "libqtest.h"

#define BA 0x080ffff4ULL /* signature straddles a scanner window */
#define OTHER_BA 0x08110000ULL
#define COMMAND (BA + 0x38)
#define MSEC 1000000LL
static char *rom, *nor, *nand;
static const char arguments[] = "serial=3 debug=0x8";

static QTestState *start_board(bool enabled)
{
    return qtest_initf("-machine 'iPod-Touch,bootrom=%s,direct-iboot=%s,"
        "nor=%s,nand=%s,boot-args=%s,boot-args-repeat=2,"
        "boot-args-interval-ms=250' -display none -audio driver=none -nic none",
        rom, rom, nor, nand, enabled ? arguments : "");
}

static void handoff(QTestState *q, uint64_t at)
{
    qtest_writel(q, at, 0x00050001);
    qtest_writel(q, at + 4, 0xc0000000);
    qtest_writel(q, at + 8, 0x08000000);
}

static void expect_arguments(QTestState *q, uint64_t at)
{
    char command[256];
    qtest_memread(q, at + 0x38, command, sizeof(command));
    g_assert_cmpstr(command, ==, arguments);
    for (size_t i = sizeof(arguments); i < sizeof(command); i++) {
        g_assert_cmpint(command[i], ==, 0);
    }
}

static void discovery_refresh_reset(void)
{
    QTestState *q = start_board(true);
    const char sentinel[] = "factory-root-is-not-command-line";
    char intact[sizeof(sentinel)];
    qtest_memwrite(q, 0x0ff10000, sentinel, sizeof(sentinel));
    qtest_clock_step(q, 1); /* missing signature must rearm quickly */
    qtest_clock_step(q, MSEC);
    handoff(q, BA);
    qtest_clock_step(q, MSEC);
    expect_arguments(q, BA);
    qtest_memread(q, 0x0ff10000, intact, sizeof(intact));
    g_assert_cmpmem(intact, sizeof(intact), sentinel, sizeof(sentinel));
    qtest_writeb(q, COMMAND, 'x');
    qtest_clock_step(q, 249 * MSEC);
    g_assert_cmpint(qtest_readb(q, COMMAND), ==, 'x');
    qtest_clock_step(q, MSEC);
    expect_arguments(q, BA);
    qtest_writeb(q, COMMAND, 'y');
    qtest_clock_step(q, 500 * MSEC); /* configured two writes, then stop */
    g_assert_cmpint(qtest_readb(q, COMMAND), ==, 'y');
    qtest_writel(q, BA, 0); /* reset must reject the stale cached pointer */
    handoff(q, OTHER_BA);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    qtest_clock_step(q, 1);
    expect_arguments(q, OTHER_BA);
    qtest_quit(q);
}

static void disabled(void)
{
    QTestState *q = start_board(false);
    handoff(q, BA);
    qtest_writeb(q, COMMAND, 'z');
    qtest_clock_step(q, 1000 * MSEC);
    g_assert_cmpint(qtest_readb(q, COMMAND), ==, 'z');
    qtest_quit(q);
}

static void unknown_handoff(void)
{
    QTestState *q = start_board(true);
    qtest_clock_step(q, 1);
    qtest_clock_step(q, 500 * MSEC); /* bounded search, no candidate */
    handoff(q, BA);
    qtest_writeb(q, COMMAND, 'z');
    qtest_clock_step(q, 1000 * MSEC);
    g_assert_cmpint(qtest_readb(q, COMMAND), ==, 'z');
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    int fd, result;
    g_autofree char *zero = g_malloc0(1048576);
    fd = g_file_open_tmp("n72-args-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    fd = g_file_open_tmp("n72-args-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("n72-args-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/boot-args/discovery-refresh-reset", discovery_refresh_reset);
    qtest_add_func("/ipod/boot-args/disabled", disabled);
    qtest_add_func("/ipod/boot-args/search-deadline", unknown_handoff);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
