/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The S5L8900 FMC (iPod touch 1G, original iPhone) on a NAND image store (hw/arm/nand-image.c): nand= naming a
 * directory whose geometry.json says nand-xor-blank-v2, blank "ff", one cs<N>.pages chip file per bank, written in
 * place. Driven through the ADM as the 1.x flash driver drives it: a command block in its data2 section, the page's
 * twelve FTL metadata bytes in data3, the data through the FMC's FIFO. No firmware executes.
 */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/bswap.h"

#define RAM 0x08000000ULL
#define ADM 0x38800000ULL
#define FMC 0x38A00000ULL
#define D1 (RAM + 0x100000)
#define D2 (RAM + 0x200000)
#define D3 (RAM + 0x300000)
#define TF (D2 + 0x1104)          /* CalmADMFMCFirmware-17's transfer block */
#define PAGE 2048
#define SPARE 64
#define STRIDE (PAGE + SPARE)

static char *rom, *nor;

static char *image_store(unsigned chips)
{
    char *dir = g_dir_make_tmp("fmc-image-XXXXXX", NULL);
    g_autofree char *geometry = g_build_filename(dir, "geometry.json", NULL);
    g_autofree char *json = g_strdup_printf(
        "{\"storage_format\": \"nand-xor-blank-v2\", \"blank\": \"ff\", \"board\": \"s5l8900\", "
        "\"chip_selects\": %u, \"page_bytes\": 2048, \"spare_bytes\": 64, \"pages_per_block\": 128, "
        "\"blocks_per_ce\": 4096}", chips);
    g_assert_true(g_file_set_contents(geometry, json, -1, NULL));
    return dir;
}

static void remove_image(char *dir)
{
    for (unsigned cs = 0; cs < 8; cs++) {
        g_autofree char *chip = g_strdup_printf("%s/cs%u.pages", dir, cs);
        unlink(chip);
    }
    g_autofree char *geometry = g_build_filename(dir, "geometry.json", NULL);
    unlink(geometry);
    g_assert_cmpint(rmdir(dir), ==, 0);
    g_free(dir);
}

/* The N45 (eight banks) on the store; the sections' addresses and the transfer block set up. */
static QTestState *start(const char *dir)
{
    QTestState *q = qtest_initf("-machine iPod-Touch-1G,bootrom=%s,iboot=%s,nand=%s,wifi=off "
                                "-drive if=pflash,format=raw,file=%s -display none -audio driver=none",
                                rom, rom, dir, nor);
    qtest_writel(q, ADM + 0x84, D1);
    qtest_writel(q, ADM + 0x88, D2);
    qtest_writel(q, ADM + 0x8c, D3);
    qtest_writel(q, TF + 4, bswap32(D1));
    qtest_writel(q, TF + 8, bswap32(D2));
    qtest_writel(q, TF + 0xc, bswap32(D3));
    return q;
}

/* One ADM command on one page (or, for an erase, the block holding it). */
static void command(QTestState *q, uint32_t cmd, unsigned bank, uint32_t page)
{
    qtest_writel(q, TF + 0x24, cmd);
    qtest_writew(q, TF + 0x28, bswap16(1));
    qtest_writeb(q, TF + 0x34, bank);
    qtest_writeb(q, TF + 0x44, bank);
    qtest_writel(q, TF + 0x244, bswap32(page));
    qtest_writel(q, ADM + 4, 2);
    qtest_writel(q, ADM + 4, 0);
}

/* Reads (bank, page): its data, its whole spare, and whether the ADM reported it clean (erased). */
static bool read_page(QTestState *q, unsigned bank, uint32_t page, uint8_t *data, uint8_t *spare)
{
    command(q, 0x300, bank, page);
    bool clean = qtest_readl(q, ADM + 0x38) == 0xfe;
    qtest_memread(q, D3, spare, SPARE);
    for (unsigned i = 0; i < PAGE / 4; i++) {
        uint32_t w = qtest_readl(q, FMC + 0x80);
        memcpy(data + 4 * i, &w, 4);
    }
    return clean;
}

static void program(QTestState *q, unsigned bank, uint32_t page, const uint8_t *data, const uint8_t *meta)
{
    qtest_memwrite(q, D3, meta, 12);
    command(q, 0x500, bank, page);
    for (unsigned i = 0; i < PAGE / 4; i++) {
        uint32_t w;
        memcpy(&w, data + 4 * i, 4);
        qtest_writel(q, FMC + 0x80, w);
    }
    for (unsigned i = 0; i < 4; i++) {  /* the pad descriptor after the page */
        qtest_writel(q, FMC + 0x80, 0);
    }
}

static void assert_erased(QTestState *q, unsigned bank, uint32_t page)
{
    uint8_t data[PAGE], spare[SPARE];
    g_assert_true(read_page(q, bank, page, data, spare));
    for (unsigned i = 0; i < PAGE; i++) {
        g_assert_cmphex(data[i], ==, 0xff);
    }
    for (unsigned i = 0; i < SPARE; i++) {
        g_assert_cmphex(spare[i], ==, 0xff);
    }
}

static void assert_page(QTestState *q, unsigned bank, uint32_t page, uint8_t fill, const uint8_t *meta)
{
    uint8_t data[PAGE], spare[SPARE];
    g_assert_false(read_page(q, bank, page, data, spare));
    for (unsigned i = 0; i < PAGE; i++) {
        g_assert_cmphex(data[i], ==, fill);
    }
    g_assert_cmpmem(spare, 12, meta, 12);
    for (unsigned i = 12; i < SPARE; i++) {  /* the controller's ECC bytes: the model leaves them erased */
        g_assert_cmphex(spare[i], ==, 0xff);
    }
}

static void stored(const char *dir, unsigned bank, uint32_t page, uint8_t *record)
{
    g_autofree char *chip = g_strdup_printf("%s/cs%u.pages", dir, bank);
    int fd = open(chip, O_RDONLY);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(pread(fd, record, STRIDE, (off_t)page * STRIDE), ==, STRIDE);
    close(fd);
}

/* A hole reads erased (the ADM reports it clean); a program lands in the bank's chip file, stored XOR 0xFF, and a
 * second program without an erase only clears bits, as on the chip; an erase makes its block's pages erased again;
 * and all of it is there after the emulator is started again. */
static void image_store_program_erase_reopen(void)
{
    char *dir = image_store(8);
    uint8_t ones[PAGE], twos[PAGE], meta[12], record[STRIDE];
    memset(ones, 0x3c, sizeof(ones));
    memset(twos, 0x0f, sizeof(twos));
    for (unsigned i = 0; i < sizeof(meta); i++) {
        meta[i] = 0x10 + i;
    }
    QTestState *q = start(dir);

    assert_erased(q, 5, 7);
    program(q, 5, 7, ones, meta);
    assert_page(q, 5, 7, 0x3c, meta);
    program(q, 5, 7, twos, meta);
    assert_page(q, 5, 7, 0x3c & 0x0f, meta);
    program(q, 0, 130, ones, meta);   /* block 1 of bank 0 */
    program(q, 0, 300, ones, meta);   /* block 2 */
    assert_page(q, 0, 130, 0x3c, meta);
    command(q, 0x600, 0, 131);        /* erase: any page of block 1 */
    assert_erased(q, 0, 130);
    assert_page(q, 0, 300, 0x3c, meta);
    qtest_quit(q);

    /* On disk: the record XOR 0xFF at page * 2112 of the bank's chip file; an erased block reads back zeros. */
    stored(dir, 5, 7, record);
    g_assert_cmphex(record[0], ==, (0x3c & 0x0f) ^ 0xff);
    g_assert_cmphex(record[PAGE], ==, meta[0] ^ 0xff);
    g_assert_cmphex(record[PAGE + 12], ==, 0);
    stored(dir, 0, 130, record);
    for (unsigned i = 0; i < STRIDE; i++) {
        g_assert_cmphex(record[i], ==, 0);
    }

    q = start(dir);
    assert_page(q, 5, 7, 0x0c, meta);
    assert_page(q, 0, 300, 0x3c, meta);
    assert_erased(q, 0, 130);
    assert_erased(q, 7, 4096 * 128 - 1);
    qtest_quit(q);
    remove_image(dir);
}

/* FMDNUM is the guest's: a program word with a count outside the page ends the program (nothing past the page buffer
 * is written), and the next program works. */
static void program_count_outside_page(void)
{
    char *dir = image_store(8);
    uint8_t ones[PAGE], meta[12] = { 1, 2, 3 };
    memset(ones, 0x3c, sizeof(ones));
    QTestState *q = start(dir);
    static const uint32_t counts[] = { 0x80000000, 0x1000, 2047, 0 };

    for (unsigned i = 0; i < ARRAY_SIZE(counts); i++) {
        command(q, 0x500, 1, 9);
        qtest_writel(q, FMC + 0x30, counts[i]);
        qtest_writel(q, FMC + 0x80, 0xdeadbeef);
        qtest_writel(q, FMC + 0x80, 0xdeadbeef);
    }
    assert_erased(q, 1, 9);
    program(q, 1, 9, ones, meta);
    assert_page(q, 1, 9, 0x3c, meta);
    qtest_quit(q);
    remove_image(dir);
}

static void expect_startup_failure(const char *machine, const char *error_text)
{
    g_autofree char *drive = g_strdup_printf("if=pflash,format=raw,file=%s", nor);
    const char *argv[] = { getenv("QTEST_QEMU_BINARY"), "-machine", machine, "-drive", drive,
                     "-display", "none", "-audio", "driver=none", "-monitor", "none", "-serial", "none", "-S", NULL };
    g_autofree char *err = NULL;
    int status;
    g_assert_true(g_spawn_sync(NULL, (char **)argv, NULL, G_SPAWN_STDOUT_TO_DEV_NULL, NULL, NULL, NULL, &err, &status, NULL));
    g_assert_false(g_spawn_check_exit_status(status, NULL));
    if (!strstr(err, error_text)) {
        g_test_message("Unexpected QEMU startup error: %s", err);
    }
    g_assert_nonnull(strstr(err, error_text));
}

/* An image store is written in place: a nand-overlay beside it is refused at startup. */
static void image_store_refuses_overlay(void)
{
    char *dir = image_store(8);
    g_autofree char *overlay = g_dir_make_tmp("fmc-overlay-XXXXXX", NULL);
    g_autofree char *machine = g_strdup_printf("iPod-Touch-1G,bootrom=%s,iboot=%s,nand=%s,nand-overlay=%s,wifi=off",
                                               rom, rom, dir, overlay);
    expect_startup_failure(machine, "takes no nand-overlay");
    rmdir(overlay);
    remove_image(dir);
}

/* The N45 has eight banks: a store of four (the M68's) is not its NAND. */
static void image_store_refuses_other_geometry(void)
{
    char *dir = image_store(4);
    g_autofree char *machine = g_strdup_printf("iPod-Touch-1G,bootrom=%s,iboot=%s,nand=%s,wifi=off", rom, rom, dir);
    expect_startup_failure(machine, "\"chip_selects\" is not the model's 8");
    remove_image(dir);
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;
    fd = g_file_open_tmp("fmc-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("fmc-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom, zero, 65536, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/fmc/image-store", image_store_program_erase_reopen);
    qtest_add_func("/ipod/fmc/program-count-outside-page", program_count_outside_page);
    qtest_add_func("/ipod/fmc/image-store-refuses-overlay", image_store_refuses_overlay);
    qtest_add_func("/ipod/fmc/image-store-refuses-other-geometry", image_store_refuses_other_geometry);
    result = g_test_run();
    unlink(rom); unlink(nor);
    g_free(rom); g_free(nor);
    return result;
}
