/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Each S5L8920 board's Wi-Fi card answers behind the SDHC at 0x80000000 as
 * AppleBCMWLANBusInterfaceSDIO matches it: CMD5, the CIS MANFID and the
 * VERS_1 strings. The N88 had no card at all, so iOS 6's AppleIOPSDIO read
 * the unimplemented window and gave up ("SDIO Internal Clk Unstable").
 */
#include "qemu/osdep.h"
#include "libqtest.h"

#define SDHC 0x80000000ULL

static char *kboot;

static uint32_t command(QTestState *q, uint8_t cmd, uint32_t arg)
{
    qtest_writel(q, SDHC + 8, arg);
    qtest_writew(q, SDHC + 0x0e, cmd << 8);
    return qtest_readl(q, SDHC + 0x10);
}

static uint8_t card_read(QTestState *q, uint32_t address)
{
    return command(q, 52, address << 9);
}

static void check_card(const char *machine, uint16_t prodid, const char *rev, const char *board)
{
    QTestState *q = qtest_initf("-machine %s,kboot=%s,wifi=off -display none -audio driver=none -nic none",
                                machine, kboot);
    uint32_t cis;
    bool manfid = false, vers1 = false;

    qtest_writeb(q, SDHC + 0x2f, 1);                 /* software reset: all */
    qtest_writew(q, SDHC + 0x2c, 1);                 /* internal clock on */
    g_assert_cmphex(qtest_readw(q, SDHC + 0x2c) & 2, ==, 2);
    g_assert_cmphex(qtest_readl(q, SDHC + 0x24) & 0x00070000, ==, 0x00070000);
    g_assert_cmphex(command(q, 5, 0) & 0xf0000000, ==, 0xa0000000);
    cis = card_read(q, 9) | card_read(q, 10) << 8 | card_read(q, 11) << 16;
    for (unsigned n = 0; n < 64; n++) {
        unsigned code = card_read(q, cis), len;
        char body[256];

        if (code == 0xff) {
            break;
        }
        if (code == 0) {
            cis++;
            continue;
        }
        len = card_read(q, cis + 1);
        for (unsigned i = 0; i < len; i++) {
            body[i] = card_read(q, cis + 2 + i) ?: ' ';   /* the strings' NULs as spaces */
        }
        body[len] = 0;
        if (code == 0x20) {
            g_assert_cmphex(card_read(q, cis + 2) | card_read(q, cis + 3) << 8, ==, 0x02d0);
            g_assert_cmphex(card_read(q, cis + 4) | card_read(q, cis + 5) << 8, ==, prodid);
            manfid = true;
        } else if (code == 0x15) {
            g_assert_nonnull(strstr(body, rev));
            g_assert_nonnull(strstr(body, board));
            vers1 = true;
        }
        cis += len + 2;
    }
    g_assert_true(manfid);
    g_assert_true(vers1);
    qtest_quit(q);
}

static void n18(void) { check_card("n18", 0x4329, "s=B1", "P=N18"); }
static void n88(void) { check_card("n88", 0xa8f2, "s=D1", "P=N88"); }

int main(int argc, char **argv)
{
    /* A K48KBOOT bundle with an empty image: the trailer only. */
    static const uint8_t trailer[24] = {
        'K', '4', '8', 'K', 'B', 'O', 'O', 'T',
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40,
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
    };
    int fd, ret;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("s5l8920-kboot-XXXXXX", &kboot, NULL);
    g_assert(fd >= 0);
    g_assert(write(fd, trailer, sizeof(trailer)) == sizeof(trailer));
    close(fd);
    qtest_add_func("/s5l8920/sdio/n18-card", n18);
    qtest_add_func("/s5l8920/sdio/n88-card", n88);
    ret = g_test_run();
    unlink(kboot);
    g_free(kboot);
    return ret;
}
