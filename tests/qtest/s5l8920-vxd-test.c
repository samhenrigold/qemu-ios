/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The VXD (H.264 decoder) on every board that has one: the S5L8920's N88 and N18 map it at 0x85000000 with
 * interrupt 0x2a, as the A4's iPad and iPhone 4 do (interrupt 0x30), so AppleVXD375's power-on finds the firmware's
 * comms signature. A render completes after its decode time, not inside the kick: the completion and the MTX
 * interrupt wait for it, a message's after the one before it, and a picture's slices complete once, at its last.
 */
#include "qemu/osdep.h"
#include "libqtest.h"

#define VXD             0x85000000
#define MTX_ENABLE      0x000
#define MTX_KICK        0x080
#define INT_STATUS      0x608
#define INT_ENABLE      0x610
#define   INT_MTX       0x4000
#define TO_HOST_BUF     0x2fe4
#define TO_HOST_WR      0x2fec
#define SIGNATURE       0x2fe0
#define TO_MTX_BUF      0x2ff0
#define TO_MTX_WR       0x2ffc

static char *kboot;

static uint32_t rd(QTestState *q, uint32_t off) { return qtest_readl(q, VXD + off); }
static void wr(QTestState *q, uint32_t off, uint32_t v) { qtest_writel(q, VXD + off, v); }

/*
 * A message the firmware takes but that isn't a render it can decode: it completes after the setup time alone.
 * flags is word 7: 0x800 waives the reply (a picture's other slices), 0x4000 asks for the host's interrupt (its last).
 */
static void post_render(QTestState *q, unsigned *w, uint32_t fence, uint32_t flags)
{
    uint32_t ring = 0x2000 + (rd(q, TO_MTX_BUF) >> 16);
    uint32_t msg[8] = { 0x81 << 8 | 32, 0, 0, 0, fence, 0, 0, flags };   /* RENDER, 32 bytes, no command buffer */

    for (int i = 0; i < 8; i++) {
        wr(q, ring + 4 * (*w + i), msg[i]);
    }
    *w += 8;
    wr(q, TO_MTX_WR, *w);
    wr(q, MTX_KICK, 1);
}

static void check(const char *machine)
{
    QTestState *q = qtest_initf("-machine %s,kboot=%s -display none -audio driver=none -nic none", machine, kboot);
    unsigned w = 0;

    wr(q, MTX_ENABLE, 1);
    g_assert_cmphex(rd(q, SIGNATURE), ==, 0xa5a5a5a5);
    wr(q, INT_ENABLE, INT_MTX);

    /* A two-slice picture: one completion, at the end of its last slice. */
    post_render(q, &w, 7, 0x801);
    post_render(q, &w, 7, 0x4800);
    g_assert_cmpuint(rd(q, TO_HOST_WR), ==, 0);              /* nothing completes inside the kick */
    g_assert_cmphex(rd(q, INT_STATUS) & INT_MTX, ==, 0);
    qtest_clock_step(q, 30000);                               /* the first slice's setup: no reply for it */
    g_assert_cmpuint(rd(q, TO_HOST_WR), ==, 0);
    g_assert_cmphex(rd(q, INT_STATUS) & INT_MTX, ==, 0);
    qtest_clock_step(q, 30000);                               /* the last slice, after the first */
    g_assert_cmpuint(rd(q, TO_HOST_WR), ==, 3);
    g_assert_cmphex(rd(q, INT_STATUS) & INT_MTX, ==, INT_MTX);
    uint32_t to_host = 0x2000 + (rd(q, TO_HOST_BUF) >> 16);
    g_assert_cmphex(rd(q, to_host) >> 8, ==, 0xc0);            /* CMD_COMPLETED with the picture's fence */
    g_assert_cmpuint(rd(q, to_host + 4), ==, 7);
    /* A message that doesn't waive its reply gets one. */
    post_render(q, &w, 8, 0);
    qtest_clock_step(q, 30000);
    g_assert_cmpuint(rd(q, TO_HOST_WR), ==, 6);
    g_assert_cmpuint(rd(q, to_host + 16), ==, 8);
    qtest_quit(q);
}

static void n88(void) { check("n88"); }
static void n18(void) { check("n18"); }
static void ipad1(void) { check("ipad1"); }

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
    qtest_add_func("/s5l8920/vxd/n88", n88);
    qtest_add_func("/s5l8920/vxd/n18", n18);
    qtest_add_func("/s5l8920/vxd/ipad1", ipad1);
    ret = g_test_run();
    unlink(kboot);
    g_free(kboot);
    return ret;
}
