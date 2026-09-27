/*
 * S5L8930 CDMA engine self-check: one memory-to-memory descriptor and one
 * routed through an AES context, driven the way AppleCDMA does it.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "libqtest.h"

static QTestState *qts;

#define CDMA_BASE   0x87000000
#define AES_BASE    0x87800000
#define CH          2
#define CH_BASE     (CDMA_BASE + (CH << 12))
#define SRC         0x41000000
#define DST         0x41200000
#define RING        0x41100000

static void write_desc(QTestState *qts, uint32_t at, uint32_t next,
                       uint32_t flags, uint32_t addr, uint32_t len)
{
    uint32_t d[8] = { next, flags, addr, len, 0, 0, 0, 0 };
    qtest_memwrite(qts, at, d, sizeof(d));
}

static uint32_t run_chain(QTestState *qts, uint32_t first_desc)
{
    uint32_t st;

    qtest_writel(qts, CDMA_BASE + 0x00, 1u << CH);       /* enable channel */
    qtest_writel(qts, CH_BASE + 0x00, 2);                /* reset */
    qtest_writel(qts, CH_BASE + 0x04, 2 | (2 << 2));     /* mem -> device, 4 B */
    qtest_writel(qts, CH_BASE + 0x08, DST);
    qtest_writel(qts, CH_BASE + 0x14, first_desc);
    qtest_writel(qts, CH_BASE + 0x00, 0x19);             /* go */
    st = qtest_readl(qts, CH_BASE + 0x00);
    g_assert_cmphex(st & 0x30000, ==, 0);                /* not running */
    g_assert_cmphex(st & 0x80000, ==, 0x80000);          /* done */
    g_assert_cmphex(st & 0x40000, ==, 0);                /* no error */
    qtest_writel(qts, CH_BASE + 0x00, st | 0x180000);    /* ack, as the ISR */
    g_assert_cmphex(qtest_readl(qts, CH_BASE + 0x00) & 0x3C0000, ==, 0);
    return st;
}

static void test_m2m(void)
{
    uint8_t in[64], out[64];

    for (int i = 0; i < sizeof(in); i++) {
        in[i] = i * 7;
    }
    qtest_memwrite(qts, SRC, in, sizeof(in));
    write_desc(qts, RING, RING + 32, 0x3, SRC, 32);
    write_desc(qts, RING + 32, RING + 64, 0x103, SRC + 32, 32);
    run_chain(qts, RING);
    qtest_memread(qts, DST, out, sizeof(out));
    g_assert_cmpmem(in, sizeof(in), out, sizeof(out));
    /* Current descriptor advanced past the last one. */
    g_assert_cmphex(qtest_readl(qts, CH_BASE + 0x14), ==, RING + 64);
}

static void test_aes(void)
{
    /* AES-128-CBC decrypt of a zero block under a zero key and IV. */
    static const uint8_t expect[16] = {
        0x14, 0x0f, 0x0f, 0x10, 0x11, 0xb5, 0x22, 0x3d,
        0x79, 0x58, 0x77, 0x17, 0xff, 0xd9, 0xec, 0x3a,
    };
    uint8_t zero[16] = { 0 }, out[16];
    uint32_t ctx1 = AES_BASE + (1 << 12);

    qtest_memwrite(qts, SRC, zero, sizeof(zero));
    for (int i = 0; i < 4; i++) {
        qtest_writel(qts, ctx1 + 0x10 + 4 * i, 0);
        qtest_writel(qts, ctx1 + 0x20 + 4 * i, 0);
    }
    /* channel 2, enable, 128-bit custom key, decrypt */
    qtest_writel(qts, ctx1 + 0x00, (CH << 8) | 0x20000 | 0x100000);
    write_desc(qts, RING, RING + 32, 0x30103, SRC, 16);
    run_chain(qts, RING);
    qtest_memread(qts, DST, out, sizeof(out));
    g_assert_cmpmem(expect, sizeof(expect), out, sizeof(out));
}

int main(int argc, char **argv)
{
    g_autofree char *kboot = NULL;
    /* Minimal K48KBOOT bundle: empty image, trailer only. */
    static const uint8_t trailer[24] = {
        'K', '4', '8', 'K', 'B', 'O', 'O', 'T',
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x40,
        0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x00,
    };
    int fd, ret;

    g_test_init(&argc, &argv, NULL);
    fd = g_file_open_tmp("ipad1-kboot-XXXXXX", &kboot, NULL);
    g_assert(fd >= 0);
    g_assert(write(fd, trailer, sizeof(trailer)) == sizeof(trailer));
    close(fd);

    qts = qtest_initf("-machine ipad1,kboot=%s", kboot);
    qtest_add_func("/ipad1/cdma/m2m", test_m2m);
    qtest_add_func("/ipad1/cdma/aes", test_aes);
    ret = g_test_run();
    qtest_quit(qts);
    unlink(kboot);
    return ret;
}
