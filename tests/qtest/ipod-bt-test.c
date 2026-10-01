/* SPDX-License-Identifier: GPL-2.0-or-later */
/* BCM address programming through the production N72 UART1/H4 transport. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define UART 0x3db00000ULL
#define MSEC 1000000LL
static char *rom, *nor, *nand;
static const uint8_t default_addr[] = { 0x66, 0x55, 0x44, 0x33, 0x22, 0x02 };
static const uint8_t address[] = { 0x53, 0x81, 0xc1, 0xea, 0xd9, 0x02 };

static QTestState *start_board(bool incoming)
{
    QTestState *q = qtest_initf("-machine 'iPod-Touch,bootrom=%s,nor=%s,nand=%s,"
        "boot-args=,bt-latency-us=1000' %s -display none -audio driver=none -nic none",
        rom, nor, nand, incoming ? "-incoming defer" : "");
    if (!incoming) {
        qtest_writel(q, UART + 4, 1); /* interrupt/poll receive, no DMA */
        qtest_writel(q, UART + 8, 7); /* enable FIFO, clear Tx/Rx */
    }
    return q;
}

static void send_bytes(QTestState *q, const uint8_t *bytes, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        qtest_writeb(q, UART + 0x20, bytes[i]);
    }
}

static void send_command(QTestState *q, uint16_t opcode,
                         const uint8_t *payload, unsigned count)
{
    const uint8_t header[] = { 1, opcode & 255, opcode >> 8, count };
    send_bytes(q, header, sizeof(header));
    send_bytes(q, payload, count);
}

static void expect_reply(QTestState *q, uint16_t opcode, uint8_t status,
                         const uint8_t *parameters, unsigned count)
{
    const uint8_t header[] = { 4, 0x0e, 4 + count, 1,
                              opcode & 255, opcode >> 8, status };
    qtest_clock_step(q, MSEC);
    g_assert_cmpuint(qtest_readl(q, UART + 0x18) & 255, ==, sizeof(header) + count);
    for (unsigned i = 0; i < sizeof(header); i++) {
        g_assert_cmphex(qtest_readb(q, UART + 0x24), ==, header[i]);
    }
    for (unsigned i = 0; i < count; i++) {
        g_assert_cmphex(qtest_readb(q, UART + 0x24), ==, parameters[i]);
    }
    g_assert_cmpuint(qtest_readl(q, UART + 0x18) & 255, ==, 0);
}

static void expect_address(QTestState *q, const uint8_t *expected)
{
    send_command(q, 0x1009, NULL, 0);
    expect_reply(q, 0x1009, 0, expected, 6);
}

static void programming(void)
{
    QTestState *q = start_board(false);
    const uint8_t header[] = { 1, 1, 0xfc, 6 };
    expect_address(q, default_addr);
    /* Fragmented H4 command must not be executed until the full payload. */
    send_bytes(q, header, sizeof(header));
    send_bytes(q, address, 3);
    qtest_clock_step(q, 2 * MSEC);
    g_assert_cmpuint(qtest_readl(q, UART + 0x18) & 255, ==, 0);
    send_bytes(q, address + 3, 3);
    qtest_clock_step(q, MSEC - 1);
    g_assert_cmpuint(qtest_readl(q, UART + 0x18) & 255, ==, 0);
    expect_reply(q, 0xfc01, 0, NULL, 0);
    expect_address(q, address);
    /* BlueTool resets HCI after provisioning; it must keep the address. */
    send_command(q, 0x0c03, NULL, 0);
    expect_reply(q, 0x0c03, 0, NULL, 0);
    expect_address(q, address);
    send_command(q, 0xfc01, address, 5);
    expect_reply(q, 0xfc01, 0x12, NULL, 0);
    expect_address(q, address);
    send_command(q, 0xfc01, NULL, 0);
    expect_reply(q, 0xfc01, 0x12, NULL, 0);
    expect_address(q, address);
    send_command(q, 0x1009, address, 1);
    expect_reply(q, 0x1009, 0x12, NULL, 0);
    expect_address(q, address);
    qtest_quit(q);
}

static void board_reset(void)
{
    QTestState *q = start_board(false);
    const uint8_t partial[] = { 1, 1, 0xfc, 6, 0xaa };
    send_command(q, 0xfc01, address, 6); /* response still queued */
    send_bytes(q, partial, sizeof(partial));
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    qtest_writel(q, UART + 4, 1);
    qtest_writel(q, UART + 8, 7);
    qtest_clock_step(q, 2 * MSEC);
    g_assert_cmpuint(qtest_readl(q, UART + 0x18) & 255, ==, 0);
    expect_address(q, default_addr);
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
        if (complete) { return; }
        g_usleep(1000);
    }
    g_assert_not_reached();
}

static void restored_address(void)
{
    g_autofree char *state = NULL;
    g_autofree char *uri = NULL;
    int fd = g_file_open_tmp("n72-bt-state-XXXXXX", &state, NULL);
    QTestState *from = start_board(false), *to;
    const uint8_t partial_read[] = { 1, 9, 0x10 };
    const uint8_t end_read = 0;
    g_assert_cmpint(fd, >=, 0); close(fd);
    uri = g_strdup_printf("file:%s", state);
    send_command(from, 0xfc01, address, 6);
    expect_reply(from, 0xfc01, 0, NULL, 0);
    send_bytes(from, partial_read, sizeof(partial_read));
    qtest_qmp_assert_success(from, "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(from);
    qtest_quit(from);
    to = start_board(true);
    qtest_qmp_assert_success(to, "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    wait_migration(to);
    send_bytes(to, &end_read, 1);
    expect_reply(to, 0x1009, 0, address, 6);
    expect_address(to, address);
    qtest_quit(to);
    unlink(state);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;
    fd = g_file_open_tmp("n72-bt-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n72-bt-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("n72-bt-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/bluetooth/address-programming", programming);
    qtest_add_func("/ipod/bluetooth/board-reset", board_reset);
    qtest_add_func("/ipod/bluetooth/restored-address", restored_address);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
