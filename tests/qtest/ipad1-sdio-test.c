/* SPDX-License-Identifier: GPL-2.0-or-later */
/* The production K48 SDHCI/card wiring must survive host bridge policy. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define SDHC 0x80000000ULL
static char *rom;

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

static void assert_card(QTestState *q)
{
    g_assert_cmphex(qtest_readl(q, SDHC + 0x24) & 0x00070000, ==, 0x00070000);
    g_assert_cmphex(command(q, 5, 0) & 0xf0000000, ==, 0xa0000000);
    uint32_t cis = card_read(q, 9) | card_read(q, 10) << 8 | card_read(q, 11) << 16;
    bool found = false;
    for (unsigned n = 0; n < 64; n++) {
        unsigned code = card_read(q, cis);
        if (code == 0xff) { break; }
        if (code == 0) { cis++; continue; }
        unsigned len = card_read(q, cis + 1);
        if (code == 0x20) {
            g_assert_cmpuint(len, ==, 4);
            g_assert_cmphex(card_read(q, cis + 2) | card_read(q, cis + 3) << 8, ==, 0x02d0);
            g_assert_cmphex(card_read(q, cis + 4) | card_read(q, cis + 5) << 8, ==, 0x4329);
            found = true;
            break;
        }
        cis += len + 2;
    }
    g_assert_true(found);
}

static void check_board(const char *options, const char *netdev, bool bridge)
{
    QTestState *q = qtest_initf("-machine ipad1,bootrom=%s%s -display none "
                               "-audio driver=none -nic none %s", rom, options, netdev);
    assert_card(q);
    QDict *r = qtest_qmp(q, "{'execute':'human-monitor-command','arguments':"
                          "{'command-line':'info network'}}");
    g_assert_nonnull(qdict_get(r, "return"));
    g_assert_cmpint(strstr(qdict_get_str(r, "return"), "model=ipodtouch.sdio") != NULL, ==, bridge);
    qobject_unref(r);
    r = qtest_qmp(q, "{'execute':'qom-set','arguments':{'path':'/machine',"
                     "'property':'wifi','value':false}}");
    g_assert_nonnull(qdict_get(r, "error"));
    qobject_unref(r);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    assert_card(q);
    qtest_quit(q);
}

/* The CISTPL_FUNCE type-4 MAC the card reports, which AppleBCMWLAN reads. */
static void assert_cis_mac(QTestState *q, const uint8_t *mac)
{
    uint32_t cis = card_read(q, 9) | card_read(q, 10) << 8 | card_read(q, 11) << 16;
    bool found = false;
    for (unsigned n = 0; n < 64; n++) {
        unsigned code = card_read(q, cis);
        if (code == 0xff) { break; }
        if (code == 0) { cis++; continue; }
        unsigned len = card_read(q, cis + 1);
        if (code == 0x22 && len == 8 && card_read(q, cis + 2) == 4 &&
            card_read(q, cis + 3) == 6) {
            for (unsigned j = 0; j < 6; j++) {
                g_assert_cmphex(card_read(q, cis + 4 + j), ==, mac[j]);
            }
            found = true;
            break;
        }
        cis += len + 2;
    }
    g_assert_true(found);
}

static void wifi_mac(void)
{
    static const uint8_t placeholder[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
    static const uint8_t set[6] = {0x02, 0x9f, 0xef, 0x8e, 0x4a, 0xf8};
    QTestState *q = qtest_initf("-machine ipad1,bootrom=%s -display none "
                               "-audio driver=none -nic none", rom);
    command(q, 5, 0);
    assert_cis_mac(q, placeholder);
    qtest_quit(q);

    q = qtest_initf("-machine ipad1,bootrom=%s,wifi-mac=02:9f:ef:8e:4a:f8 "
                    "-display none -audio driver=none -nic none", rom);
    command(q, 5, 0);
    assert_cis_mac(q, set);
    QDict *r = qtest_qmp(q, "{'execute':'qom-get','arguments':{'path':'/machine',"
                         "'property':'wifi-mac'}}");
    g_assert_cmpstr(qdict_get_str(r, "return"), ==, "02:9f:ef:8e:4a:f8");
    qobject_unref(r);
    /* A running card's identity cannot change underneath its driver. */
    r = qtest_qmp(q, "{'execute':'qom-set','arguments':{'path':'/machine',"
                     "'property':'wifi-mac','value':'02:11:22:33:44:55'}}");
    g_assert_nonnull(qdict_get(r, "error"));
    qobject_unref(r);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    command(q, 5, 0);
    assert_cis_mac(q, set);
    qtest_quit(q);
}

static void default_bridge(void) { check_board("", "", true); }
static void bridge_off(void) { check_board(",wifi=off", "", false); }
static void bridge_off_with_backend(void)
{
    check_board(",wifi=off", "-netdev user,id=wifi0", false);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(65536);
    int fd = g_file_open_tmp("k48-sdio-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 65536, NULL));
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipad1/sdio/default-bridge-card", default_bridge);
    qtest_add_func("/ipad1/sdio/bridge-off-card-reset", bridge_off);
    qtest_add_func("/ipad1/sdio/bridge-off-existing-backend", bridge_off_with_backend);
    qtest_add_func("/ipad1/sdio/wifi-mac", wifi_mac);
    int result = g_test_run();
    unlink(rom); g_free(rom);
    return result;
}
