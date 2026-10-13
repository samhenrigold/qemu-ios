/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The iPad's Wi-Fi + 3G model (baseband=on) as iBoot sees it before the kernel: the radio-present strap
 * (GPIO 0x607) and the modem's AT on uart1, which iBoot reads through its FIFO, taking the Rx count as
 * UFSTAT[3:0] | full << 4, and gives each reply line 1 s (iBoot-1219 0x5ff09340). No guest firmware runs:
 * the zero ROM only satisfies the board.
 */
#include "qemu/osdep.h"
#include "libqtest.h"

#define GPIO_607 (0xbfa00000ULL + (6 * 8 + 7) * 4)
#define UART1 0x82600000ULL
#define UFCON 0x08
#define UFSTAT 0x18
#define UTXH 0x20
#define URXH 0x24

static char *rom_path;

static QTestState *start_board(const char *options)
{
    return qtest_initf("-machine ipad1,bootrom=%s%s -display none -audio driver=none -nic none",
                       rom_path, options);
}

static void test_strap(void)
{
    QTestState *q = start_board("");

    g_assert_cmphex(qtest_readl(q, GPIO_607) & 1, ==, 0);    /* "Radio not detected." */
    qtest_quit(q);
    q = start_board(",baseband=on");
    g_assert_cmphex(qtest_readl(q, GPIO_607) & 1, ==, 1);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(q, GPIO_607) & 1, ==, 1);    /* a strap, across resets */
    qtest_quit(q);
}

/* One AT command on uart1 and its reply, polled as iBoot polls; the virtual microseconds it took. */
static int64_t at_exchange(QTestState *q, const char *cmd, GString *reply)
{
    int64_t us = 0;

    qtest_writel(q, UART1 + UFCON, 1);
    for (const char *p = cmd; *p; p++) {
        qtest_writel(q, UART1 + UTXH, (uint8_t)*p);
    }
    while (!g_str_has_suffix(reply->str, "\r\nOK\r\n") && us < 5000000) {
        uint32_t fstat = qtest_readl(q, UART1 + UFSTAT);
        unsigned count = (fstat & 0xf) | ((fstat >> 4) & 0x10);     /* iBoot-1219 0x5ff0c4d2 */

        if (!count) {
            qtest_clock_step(q, 50000);
            us += 50;
        }
        while (count--) {
            g_string_append_c(reply, qtest_readl(q, UART1 + URXH));
        }
    }
    return us;
}

static void test_boot_uart(void)
{
    QTestState *q = start_board(",baseband=on");
    g_autoptr(GString) reply = g_string_new("");

    at_exchange(q, "at\r", reply);
    g_assert_cmpstr(reply->str, ==, "\r\nOK\r\n");
    g_string_truncate(reply, 0);
    at_exchange(q, "at+cgsn;\r", reply);
    g_assert_nonnull(strstr(reply->str, "000000001234569"));
    /* A radio nvram block: 512 bytes as hex on one line, inside the second iBoot gives it (with a 256-byte
     * FIFO the count read 0 past 15 bytes, and the line never came). */
    g_string_truncate(reply, 0);
    g_assert_cmpint(at_exchange(q, "at+xdrv=9,1,0\r", reply), <, 1000000);
    g_assert_true(g_str_has_prefix(reply->str, "\r\n+XDRV: 9,1,0,0,"));
    g_assert_cmpuint(reply->len, >, 1024);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_autofree char *rom = g_malloc0(65536);
    int fd = g_file_open_tmp("ipad1-qtest-rom-XXXXXX", &rom_path, NULL);
    int result;

    g_assert_cmpint(fd, >=, 0);
    close(fd);
    g_assert_true(g_file_set_contents(rom_path, rom, 65536, NULL));
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipad1/baseband/radio-strap", test_strap);
    qtest_add_func("/ipad1/baseband/boot-uart", test_boot_uart);
    result = g_test_run();
    unlink(rom_path);
    g_free(rom_path);
    return result;
}
