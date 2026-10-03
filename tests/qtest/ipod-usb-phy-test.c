/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Actual N45/N72 PHY -> OTG wiring and TCP DMA; no guest code executes. */
#include "qemu/osdep.h"
#include "libqtest.h"
#include "qemu/sockets.h"
#include "qobject/qdict.h"

#define OTG 0x38400000ULL
#define PHY 0x3c400000ULL
#define VIC 0x38e00000ULL
#define DMA 0x22021000ULL /* SRAM on both boards */
#define USB_IRQ (1u << 19)
static char *rom, *rom1g, *nor, *nand;
static bool n45;
static uint8_t in_bytes[64];   /* the last IN transaction's data */

static int listener(unsigned *port)
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = { .sin_family = AF_INET, .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    socklen_t len = sizeof(addr);
    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(bind(fd, (void *)&addr, sizeof(addr)), ==, 0);
    g_assert_cmpint(getsockname(fd, (void *)&addr, &len), ==, 0);
    g_assert_cmpint(listen(fd, 1), ==, 0);
    *port = ntohs(addr.sin_port);
    return fd;
}

static int accept_peer(int listener)
{
    int peer;
    uint8_t byte;
    do {
        peer = accept(listener, NULL, NULL);
        g_assert_cmpint(peer, >=, 0);
        /* Realize may dial before whole-machine reset closes that socket.
         * Consume its EOF; the model redials using its normal retry timer. */
        if (recv(peer, &byte, 1, MSG_PEEK | MSG_DONTWAIT) != 0) { break; }
        close(peer);
    } while (true);
    struct timeval timeout = { .tv_sec = 3 };
    setsockopt(peer, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    return peer;
}

static QTestState *start_board(int *peer, int *listenfd, bool incoming)
{
    unsigned port;
    int fd = listener(&port);
    QTestState *q;
    if (n45) {
        q = qtest_initf("-machine iPod-Touch-1G,bootrom=%s,iboot=%s,nand=%s,"
                       "wifi=off,usb-tcp-addr=127.0.0.1:%u -drive if=pflash,format=raw,file=%s "
                       "-display none -audio driver=none %s", rom1g, rom1g, nand, port, nor,
                       incoming ? "-incoming defer" : "");
    } else {
        q = qtest_initf("-machine iPod-Touch,bootrom=%s,nor=%s,nand=%s,"
                       "usb-tcp-addr=127.0.0.1:%u -display none -audio driver=none -nic none %s",
                       rom, nor, nand, port, incoming ? "-incoming defer" : "");
    }
    *peer = accept_peer(fd);
    *listenfd = fd;
    return q;
}

static int xfer(int fd, uint8_t ep, uint8_t flags, const uint8_t *data, unsigned size)
{
    uint8_t header[5] = { 0, ep, flags, size, size >> 8 };
    uint8_t result[5];
    g_assert_cmpint(send(fd, header, sizeof(header), 0), ==, sizeof(header));
    if (!(ep & 0x80) && size) {
        g_assert_cmpint(send(fd, data, size, 0), ==, size);
    }
    g_assert_cmpint(recv(fd, result, sizeof(result), MSG_WAITALL), ==, sizeof(result));
    int16_t length = (uint16_t)result[3] | (uint16_t)result[4] << 8;
    if ((ep & 0x80) && length > 0) {
        g_assert_cmpint(length, <=, sizeof(in_bytes));
        g_assert_cmpint(recv(fd, in_bytes, length, MSG_WAITALL), ==, length);
    }
    return length;
}

static void arm_out(QTestState *q)
{
    qtest_writel(q, OTG + 0xb14, DMA);
    qtest_writel(q, OTG + 0xb10, 0x20080040);
    qtest_writel(q, OTG + 0xb00, 0x84000000);
}

/* The driver's EP0 IN data stage: DIEPDMA0, DIEPTSIZ0 (one packet), EPEna|CNAK. */
static void arm_ep0_in(QTestState *q, const uint8_t *data, unsigned size)
{
    qtest_memwrite(q, DMA + 0x100, data, size);
    qtest_writel(q, OTG + 0x914, DMA + 0x100);
    qtest_writel(q, OTG + 0x910, (1u << 19) | size);
    qtest_writel(q, OTG + 0x900, 0x84000000);
}

/*
 * The host times out a GET_DESCRIPTOR(device), and the guest arms its reply
 * only afterwards. A bus reset and a new SETUP follow. The core NAKs EP0 IN
 * after a SETUP until the driver arms with CNAK, so the late reply must not
 * answer the new request -- it did, and the control pipe then ran one reply
 * behind (usbmuxd: "Short configuration 0 (-1 of 512)"). A STALL left from
 * the abandoned request is cleared by the SETUP too.
 */
static void late_ep0_reply_after_reset(void)
{
    int fd, listener;
    QTestState *q = start_board(&fd, &listener, false);
    uint8_t get_dev[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };
    uint8_t get_cfg[8] = { 0x80, 6, 0, 2, 0, 0, 9, 0 };
    uint8_t stale[18] = { 18, 1, 0, 2, 0, 0, 0, 64, 0xac, 5, 0x9a, 0x12, 0, 0, 1, 2, 3, 4 };
    uint8_t fresh[9] = { 9, 2, 39, 0, 1, 1, 0, 0xc0, 250 };

    arm_out(q);
    g_assert_cmpint(xfer(fd, 0, 1, get_dev, sizeof(get_dev)), ==, 8);
    arm_ep0_in(q, stale, sizeof(stale));          /* too late: the host gave up */
    g_assert_cmpint(xfer(fd, 0, 2, NULL, 0), ==, 0);   /* bus reset */
    xfer(fd, 0, 4, NULL, 0);                      /* enumeration done (NAKs) */
    arm_out(q);
    g_assert_cmpint(xfer(fd, 0, 1, get_cfg, sizeof(get_cfg)), ==, 8);
    g_assert_cmpint(xfer(fd, 0x80, 0, NULL, 9), ==, -2);
    arm_ep0_in(q, fresh, sizeof(fresh));
    g_assert_cmpint(xfer(fd, 0x80, 0, NULL, 9), ==, 9);
    g_assert_cmpmem(in_bytes, 9, fresh, sizeof(fresh));

    /* The driver STALLs a request the host has already abandoned. */
    arm_out(q);
    g_assert_cmpint(xfer(fd, 0, 1, get_cfg, sizeof(get_cfg)), ==, 8);
    qtest_writel(q, OTG + 0x900, 1u << 21);
    arm_out(q);
    g_assert_cmpint(xfer(fd, 0, 1, get_dev, sizeof(get_dev)), ==, 8);
    g_assert_cmpint(xfer(fd, 0x80, 0, NULL, 18), ==, -2);
    arm_ep0_in(q, stale, sizeof(stale));
    g_assert_cmpint(xfer(fd, 0x80, 0, NULL, 18), ==, 18);
    g_assert_cmpmem(in_bytes, 18, stale, sizeof(stale));
    close(fd); close(listener);
    qtest_quit(q);
}

static void reset_transfer_and_pending_irq(void)
{
    int fd, listener;
    QTestState *q = start_board(&fd, &listener, false);
    uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };
    uint8_t sentinel[8], actual[8];
    memset(sentinel, 0xa5, sizeof(sentinel));
    qtest_memwrite(q, DMA, sentinel, sizeof(sentinel));
    qtest_writel(q, OTG + 8, 0x2b);
    qtest_writel(q, OTG + 0x18, 1u << 19);
    qtest_writel(q, OTG + 0x81c, 1u << 16);
    qtest_writel(q, OTG + 0x814, 8);
    arm_out(q);
    /* Firmware quiesces the PHY while endpoint MMIO can remain armed. */
    qtest_writel(q, PHY + 8, 7);
    g_assert_cmpint(xfer(fd, 0, 1, setup, sizeof(setup)), ==, -2);
    qtest_memread(q, DMA, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), sentinel, sizeof(sentinel));
    g_assert_cmphex(qtest_readl(q, OTG + 0xb08), ==, 0);
    g_assert_cmphex(qtest_readl(q, VIC + 8) & USB_IRQ, ==, 0);
    g_assert_cmphex(qtest_readl(q, OTG + 0xb00), ==, 0x84000000);
    g_assert_cmphex(qtest_readl(q, OTG + 0xb10), ==, 0x20080040);
    /* Core transport capabilities are independent of physical PHY traffic. */
    g_assert_cmpint(xfer(fd, 0xff, 8, NULL, 12), ==, 12);
    /* The observed ROM power-up leaves other reset bits set: only PHY bit0
     * gates these transfers. Do not turn this into an all-bits-zero condition. */
    qtest_writel(q, PHY + 8, 6);
    g_assert_cmpint(xfer(fd, 0, 1, setup, sizeof(setup)), ==, sizeof(setup));
    qtest_memread(q, DMA, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), setup, sizeof(setup));
    g_assert_cmphex(qtest_readl(q, VIC + 8) & USB_IRQ, ==, USB_IRQ);
    /* Pending core interrupts are not discarded or suppressed by PHY reset. */
    qtest_writel(q, PHY + 8, 7);
    g_assert_cmphex(qtest_readl(q, VIC + 8) & USB_IRQ, ==, USB_IRQ);
    qtest_writel(q, OTG + 0xb08, 8);
    g_assert_cmphex(qtest_readl(q, VIC + 8) & USB_IRQ, ==, 0);
    arm_out(q);
    g_assert_cmpint(xfer(fd, 0, 0, setup, sizeof(setup)), ==, -2);
    g_assert_cmpint(xfer(fd, 0x80, 0, NULL, 8), ==, -2);
    g_assert_cmpint(xfer(fd, 0, 2, NULL, 0), ==, -2);
    g_assert_cmpint(xfer(fd, 0, 4, NULL, 0), ==, -2);
    g_assert_cmphex(qtest_readl(q, OTG + 0x14), ==, 0x10000000);
    /* Whole-board reset re-drives the external signal from PHY registers. */
    qtest_qmp_assert_success(q, "{'execute':'system_reset'}");
    g_assert_cmphex(qtest_readl(q, PHY + 8), ==, 0);
    close(fd);
    fd = accept_peer(listener);
    arm_out(q);
    g_assert_cmpint(xfer(fd, 0, 1, setup, sizeof(setup)), ==, sizeof(setup));
    close(fd); close(listener);
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

static void restored_phy_signal(void)
{
    int peer, listener;
    g_autofree char *state = NULL;
    int fd = g_file_open_tmp("n72-usbphy-state-XXXXXX", &state, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_autofree char *uri = g_strdup_printf("file:%s", state);
    QTestState *q = start_board(&peer, &listener, false);
    uint8_t setup[8] = { 0x80, 6, 0, 1, 0, 0, 18, 0 };
    uint8_t actual[8], sentinel[8];
    memset(sentinel, 0xa5, sizeof(sentinel));
    qtest_memwrite(q, DMA, sentinel, sizeof(sentinel));
    arm_out(q);
    qtest_writel(q, PHY + 8, 7);
    qtest_qmp_assert_success(q, "{'execute':'migrate','arguments':{'uri':%s}}", uri);
    wait_migration(q);
    close(peer); close(listener); qtest_quit(q);
    q = start_board(&peer, &listener, true);
    qtest_qmp_assert_success(q, "{'execute':'migrate-incoming','arguments':{'uri':%s}}", uri);
    wait_migration(q);
    /* The socket is external state; post_load deliberately replaces it. */
    close(peer); peer = accept_peer(listener);
    g_assert_cmphex(qtest_readl(q, PHY + 8), ==, 7);
    g_assert_cmphex(qtest_readl(q, OTG + 0xb00), ==, 0x84000000);
    g_assert_cmpint(xfer(peer, 0, 1, setup, sizeof(setup)), ==, -2);
    qtest_memread(q, DMA, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), sentinel, sizeof(sentinel));
    qtest_writel(q, PHY + 8, 6);
    g_assert_cmpint(xfer(peer, 0, 1, setup, sizeof(setup)), ==, sizeof(setup));
    qtest_memread(q, DMA, actual, sizeof(actual));
    g_assert_cmpmem(actual, sizeof(actual), setup, sizeof(setup));
    close(peer); close(listener); qtest_quit(q); unlink(state);
}

static void n45_transfer(void)
{
    n45 = true;
    reset_transfer_and_pending_irq();
    n45 = false;
}

static void n45_restore(void)
{
    n45 = true;
    restored_phy_signal();
    n45 = false;
}

int main(int argc, char **argv)
{
    g_autofree char *zero = g_malloc0(1048576);
    int fd, result;
    fd = g_file_open_tmp("n72-usbphy-rom-XXXXXX", &rom, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n72-usbphy-nor-XXXXXX", &nor, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    fd = g_file_open_tmp("n45-usbphy-rom-XXXXXX", &rom1g, NULL);
    g_assert_cmpint(fd, >=, 0); close(fd);
    g_assert_true(g_file_set_contents(rom1g, zero, 65536, NULL));
    g_assert_true(g_file_set_contents(rom, zero, 131072, NULL));
    g_assert_true(g_file_set_contents(nor, zero, 1048576, NULL));
    nand = g_dir_make_tmp("n72-usbphy-nand-XXXXXX", NULL);
    g_test_init(&argc, &argv, NULL);
    qtest_add_func("/ipod/usbphy/reset-transfers-and-pending-core-irq", reset_transfer_and_pending_irq);
    qtest_add_func("/ipod/usbphy/restored-physical-reset-signal", restored_phy_signal);
    qtest_add_func("/ipod/usbotg/late-ep0-reply-after-reset", late_ep0_reply_after_reset);
    qtest_add_func("/ipod/usbphy/n45-reset-transfers-and-pending-core-irq", n45_transfer);
    qtest_add_func("/ipod/usbphy/n45-restored-physical-reset-signal", n45_restore);
    result = g_test_run();
    unlink(rom); unlink(rom1g); unlink(nor); rmdir(nand);
    g_free(rom); g_free(rom1g); g_free(nor); g_free(nand);
    return result;
}
