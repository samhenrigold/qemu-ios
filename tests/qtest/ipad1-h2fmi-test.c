/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Blank raw flash, no FTL seed and no guest instructions. Reproduce the stock
 * IOP's normal-read -> raw-read register sequence seen during initial BBT scan. */
#include "qemu/osdep.h"
#include "libqtest.h"

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

static void program(QTestState *qts, unsigned page, uint32_t value)
{
    qtest_writel(qts, FMI + 4, 0);
    qtest_writel(qts, FMI + 0x0c, 2);
    qtest_writel(qts, FMI + 0x34, 0);
    qtest_writel(qts, FMC + 0x0c, 1);
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
