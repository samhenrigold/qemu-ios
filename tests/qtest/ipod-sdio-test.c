/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Factory CIS identity through the production N72 host controller, not a stub. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"

#define SDIO 0x38d00000ULL
static char *rom, *nor, *nand;
static const uint8_t legacy[6] = {0x00, 0x23, 0x32, 0x6e, 0xaa, 0x10};
static const uint8_t provisioned[6] = {0x02, 0x9f, 0xef, 0x8e, 0x4a, 0xf8};

static QTestState *start_board(const char *options)
{
    return qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s,wifi=on%s "
                      "-display none -audio driver=none -nic none", rom, nor, nand, options);
}

static uint8_t card_read(QTestState *q, uint32_t address)
{
    qtest_writel(q, SDIO + 0xc, address << 9);
    qtest_writel(q, SDIO + 8, (1u << 31) | 52);
    return qtest_readl(q, SDIO + 0x20);
}

static void assert_factory_mac(QTestState *q, const uint8_t *mac)
{
    uint32_t cis = card_read(q, 9) | card_read(q, 10) << 8 | card_read(q, 11) << 16;
    bool found = false;
    for (unsigned i = 0; i < 64; i++) {
        unsigned code = card_read(q, cis), len;
        if (code == 0xff) { break; }
        if (code == 0) { cis++; continue; }
        len = card_read(q, cis + 1);
        if (code == 0x22 && len == 8 && card_read(q, cis + 2) == 4 && card_read(q, cis + 3) == 6) {
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

static void default_identity(void)
{
    QTestState *q = start_board("");
    assert_factory_mac(q, legacy);
    qtest_quit(q);
}

static void provisioned_identity(void)
{
    QTestState *q = start_board(",wifi-mac=02:9f:ef:8e:4a:f8");
    assert_factory_mac(q, provisioned);
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    assert_factory_mac(q, provisioned);
    /* A running card's factory identity cannot change underneath its driver. */
    QDict *reply = qtest_qmp(q, "{'execute':'qom-set','arguments':{'path':'/machine',"
                             "'property':'wifi-mac','value':'02:11:22:33:44:55'}}");
    g_assert_nonnull(qdict_get(reply, "error"));
    qobject_unref(reply);
    assert_factory_mac(q, provisioned);
    qtest_quit(q);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;
    fd = g_file_open_tmp("n72-sdio-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n72-sdio-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("n72-sdio-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/sdio/default-cis-identity", default_identity);
    qtest_add_func("/ipod/sdio/provisioned-cis-identity-reset", provisioned_identity);
    result = g_test_run();
    unlink(rom); unlink(nor); rmdir(nand);
    g_free(rom); g_free(nor); g_free(nand);
    return result;
}
