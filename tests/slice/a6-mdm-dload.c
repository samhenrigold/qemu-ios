/* Native bootloader request, transfer boundaries, CRC rejection and isolation.
 * SLICE hw/usb/dev-mdm9x15.c define MDM_INTERFACES|MDM_MAX_FRAME|MDM_BOOT_SRAM_BASE|MDM_BOOT_SRAM_SIZE|MDM_MAX_IMAGE
 * SLICE hw/usb/dev-mdm9x15.c typedef MdmDownloadState
 * SLICE hw/usb/dev-mdm9x15.c fn mdm_fuse_serial mdm_dload_crc mdm_hdlc_encode mdm_dload_reply mdm_download_free mdm_sahara_packet mdm_sahara_begin mdm_sahara_read_image mdm_sahara_image_header mdm_sahara_image_data mdm_sahara_finish_image mdm_sahara_receive mdm_dload_boot mdm_dload_command mdm_dload_receive mdm_read
 * PKG glib-2.0
 */
#include <glib.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
typedef struct {
    char imei[16];
} IosBbCore;
typedef struct {
    IosBbCore bb;
} IosBasebandState;
typedef struct MdmDownloadState MdmDownloadState;
typedef struct {
    unsigned data_logs;
    void *reconnect_timer;
    bool reconnect_pending;
    MdmDownloadState *download;
    GQueue rx[13];
    GByteArray *tx[13];
    IosBasebandState *baseband;
} MDM9x15State;
#define USB_DEVICE(x) (x)
#define USB_TOKEN_IN 1
static void mdm_log(MDM9x15State *s, const char *k, unsigned a, unsigned b,
                    unsigned c)
{
}
static void *usb_ep_get(void *d, int p, int n)
{
    return d;
}
static void usb_wakeup(void *p, int n)
{
}
static uint16_t lduw_le_p(const void *p)
{
    const uint8_t *b = p;
    return b[0] | b[1] << 8;
}
static void stl_le_p(void *p, uint32_t v)
{
    uint8_t *b = p;
    for (unsigned i = 0; i < 4; i++) {
        b[i] = v >> (8 * i);
    }
}
static uint32_t ldl_be_p(const void *p)
{
    const uint8_t *b = p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 | b[2] << 8 | b[3];
}
static uint16_t lduw_be_p(const void *p)
{
    const uint8_t *b = p;
    return b[0] << 8 | b[1];
}
static uint32_t ldl_le_p(const void *p)
{
    const uint8_t *b = p;
    return b[0] | b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
#define QEMU_CLOCK_VIRTUAL 0
static int64_t test_clock;
static int64_t qemu_clock_get_ms(int clock)
{
    return test_clock;
}
static unsigned radio_starts;
static int64_t radio_started_at;
static int ios_bb_network_registration(IosBbCore *bb, int64_t now)
{
    radio_starts++;
    radio_started_at = now;
    return 2;
}
static void timer_mod(void *timer, int64_t deadline)
{
}
#include "slice.h"

int main(void)
{
    IosBasebandState bb = {.bb.imei = "004999010640000"};
    MDM9x15State s = {.baseband = &bb};
    uint8_t out[128];
    const uint8_t native[] = {0x7e, 0x07, 0xc7, 0x84, 0x7e};

    for (unsigned i = 0; i < 13; i++) {
        s.tx[i] = g_byte_array_new();
    }
    assert(mdm_dload_crc((const uint8_t *)"123456789", 9) == 0x906e);
    for (unsigned split = 0; split <= sizeof(native); split++) {
        mdm_dload_receive(&s, native, split);
        mdm_dload_receive(&s, native + split, sizeof(native) - split);
        unsigned n = mdm_read(&s, 0, out, sizeof(out));
        assert(n == 12 && out[0] == 0x7e && out[1] == 8);
        assert(out[2] == 9 && out[3] == 1 && out[4] == 4 && out[5] == 0);
        assert(lduw_le_p(out + 9) == mdm_dload_crc(out + 1, 8));
        assert(g_queue_is_empty(&s.rx[3]));
    }
    uint8_t bad[] = {0x7e, 7, 0xc6, 0x84, 0x7e};
    mdm_dload_receive(&s, bad, sizeof(bad));
    assert(g_queue_is_empty(&s.rx[0]));
    /* Valid escaped parameter command; split within the escape sequence. */
    uint8_t escaped[] = {0x7e, 0x7d, 0x27, 0xc7, 0x84, 0x7e};
    mdm_dload_receive(&s, escaped, 2);
    mdm_dload_receive(&s, escaped + 2, sizeof(escaped) - 2);
    assert(mdm_read(&s, 0, out, sizeof(out)) == 12);
    const uint8_t version[] = {0x7e, 0x0c, 0x14, 0x3a, 0x7e};
    mdm_dload_receive(&s, version, sizeof(version));
    unsigned count = mdm_read(&s, 0, out, sizeof(out));
    assert(count > 6 && out[1] == 0x0d && out[2] == 18);
    assert(!memcmp(out + 3, "QEMU MDM9x15 DLOAD", 18));
    const uint8_t serial[] = {0x7e, 0x16, 0xcf, 0x85, 0x7e};
    mdm_dload_receive(&s, serial, sizeof(serial));
    count = mdm_read(&s, 0, out, sizeof(out));
    assert(count == 10 && out[1] == 0x16 && out[2] == 32);
    uint8_t first_serial[4];
    memcpy(first_serial, out + 3, sizeof(first_serial));
    mdm_dload_receive(&s, serial, sizeof(serial));
    assert(mdm_read(&s, 0, out, sizeof(out)) == count);
    assert(!memcmp(first_serial, out + 3, sizeof(first_serial)));
    bb.bb.imei[0] = '1';
    mdm_dload_receive(&s, serial, sizeof(serial));
    mdm_read(&s, 0, out, sizeof(out));
    assert(memcmp(first_serial, out + 3, sizeof(first_serial)));
    const uint8_t chip[] = {0x7e, 0x17, 0x46, 0x94, 0x7e};
    mdm_dload_receive(&s, chip, sizeof(chip));
    count = mdm_read(&s, 0, out, sizeof(out));
    const uint8_t chip_fields[] = {0x17, 0, 64, 0xe1, 0x10, 0x6f,
                                   0,    0, 0,  0,    0};
    assert(count == 15 && !memcmp(out + 1, chip_fields, sizeof(chip_fields)));
    const uint8_t pkhash[] = {0x7e, 0x18, 0xb1, 0x6c, 0x7e};
    mdm_dload_receive(&s, pkhash, sizeof(pkhash));
    count = mdm_read(&s, 0, out, sizeof(out));
    assert(count == 39 && out[1] == 0x18 && out[2] == 1 && out[3] == 0);
    for (unsigned i = 4; i < 36; i++) {
        assert(!out[i]);
    }
    /* A valid write command is rejected, never ACKed without storage. */
    uint8_t write[] = {1, 0, 0};
    uint16_t crc = mdm_dload_crc(write, 1);
    write[1] = crc;
    write[2] = crc >> 8;
    mdm_dload_command(&s, write, sizeof(write));
    assert(mdm_read(&s, 0, out, sizeof(out)) == 7 && out[1] == 3 &&
           out[3] == 6);
    /* Overflow must discard through the next delimiter, then recover. */
    g_byte_array_set_size(s.tx[0], MDM_MAX_FRAME);
    mdm_dload_receive(&s, native + 1, sizeof(native) - 1);
    assert(g_queue_is_empty(&s.rx[0]) && !s.tx[0]->len);
    mdm_dload_receive(&s, native, sizeof(native));
    assert(mdm_read(&s, 0, out, sizeof(out)) == 12);
    const uint8_t valid_write[] = {0x7e, 0x0f, 0x2a, 0,    0,    0,    0,
                                   3,    0xab, 0xcd, 0xef, 0x3a, 0xbb, 0x7e};
    mdm_dload_receive(&s, valid_write, sizeof(valid_write));
    assert(mdm_read(&s, 0, out, sizeof(out)) == 5 && out[1] == 2);
    assert(s.download && !memcmp(s.download->ram, "\xab\xcd\xef", 3));
    assert(s.download->valid[0] == 7 && !s.download->valid[1]);
    const uint8_t bad_length[] = {0x7e, 0x0f, 0x2a, 0,    0,    0x10, 0,
                                  4,    0xab, 0xcd, 0xef, 0xab, 0xae, 0x7e};
    mdm_dload_receive(&s, bad_length, sizeof(bad_length));
    assert(mdm_read(&s, 0, out, sizeof(out)) == 7 && out[1] == 3 &&
           out[3] == 3);
    const uint8_t outside[] = {0x7e, 0x0f, 0xff, 0xff, 0xff, 0xf0, 0,
                               3,    0xab, 0xcd, 0xef, 0x2d, 0xa8, 0x7e};
    mdm_dload_receive(&s, outside, sizeof(outside));
    assert(mdm_read(&s, 0, out, sizeof(out)) == 7 && out[1] == 3 &&
           out[3] == 2);
    const uint8_t cross_end[] = {0x7e, 0x0f, 0x2a, 3,    0xff, 0xff, 0,
                                 3,    0xab, 0xcd, 0xef, 0x65, 0x42, 0x7e};
    mdm_dload_receive(&s, cross_end, sizeof(cross_end));
    assert(mdm_read(&s, 0, out, sizeof(out)) == 7 && out[1] == 3 &&
           out[3] == 2);
    assert(!s.download->valid[MDM_BOOT_SRAM_SIZE / 8 - 1]);
    assert(!mdm_dload_boot(&s, MDM_BOOT_SRAM_BASE));
    memset(s.download->ram, 0, 112);
    memset(s.download->valid, 0xff, 14);
    stl_le_p(s.download->ram, 0x844bdcd1);
    stl_le_p(s.download->ram + 4, 0x73d71034);
    stl_le_p(s.download->ram + 8, 0x15);
    stl_le_p(s.download->ram + 20, 80);
    stl_le_p(s.download->ram + 24, MDM_BOOT_SRAM_BASE + 80);
    stl_le_p(s.download->ram + 28, 32);
    stl_le_p(s.download->ram + 32, 16);
    assert(!mdm_dload_boot(&s, 0xffffffff));
    s.download->valid[13] = 0x7f;
    assert(!mdm_dload_boot(&s, MDM_BOOT_SRAM_BASE));
    s.download->valid[13] = 0xff;
    assert(mdm_dload_boot(&s, MDM_BOOT_SRAM_BASE));
    assert(s.download->loader_started);
    s.download->loader_started = false;
    stl_le_p(s.download->ram + 28, 0xffffffff);
    assert(!mdm_dload_boot(&s, MDM_BOOT_SRAM_BASE));
    assert(!s.download->loader_started);
    mdm_sahara_begin(&s);
    assert(mdm_read(&s, 0, out, sizeof(out)) == 48);
    assert(ldl_le_p(out) == 1 && ldl_le_p(out + 4) == 48);
    assert(ldl_le_p(out + 8) == 2 && ldl_le_p(out + 12) == 1);
    uint8_t hello_response[48] = {2, 0, 0, 0, 48, 0, 0, 0, 2, 0, 0, 0, 1};
    hello_response[16] = 1;
    mdm_sahara_receive(&s, hello_response, sizeof(hello_response));
    assert(!s.download->hello_accepted && g_queue_is_empty(&s.rx[0]));
    hello_response[4] = 4;
    mdm_sahara_receive(&s, hello_response, sizeof(hello_response));
    assert(!s.tx[0]->len);
    hello_response[4] = 48;
    hello_response[16] = 0;
    mdm_sahara_receive(&s, hello_response, 7);
    assert(!s.download->hello_accepted);
    mdm_sahara_receive(&s, hello_response + 7, sizeof(hello_response) - 7);
    assert(s.download->hello_accepted && !s.tx[0]->len);
    assert(mdm_read(&s, 0, out, sizeof(out)) == 20);
    assert(ldl_le_p(out) == 3 && ldl_le_p(out + 8) == 22);
    assert(ldl_le_p(out + 12) == 0 && ldl_le_p(out + 16) == 40);
    uint8_t image[48] = {23, 0, 0, 0, 3};
    stl_le_p(image + 12, 0x20000);
    stl_le_p(image + 16, 8);
    stl_le_p(image + 20, 8);
    memcpy(image + 40, "RPM-test", 8);
    mdm_sahara_receive(&s, image, 13);
    assert(g_queue_is_empty(&s.rx[0]) && !s.download->image_complete);
    mdm_sahara_receive(&s, image + 13, 27);
    assert(mdm_read(&s, 0, out, sizeof(out)) == 20);
    assert(ldl_le_p(out + 12) == 40 && ldl_le_p(out + 16) == 8);
    mdm_sahara_receive(&s, image + 40, 3);
    assert(g_queue_is_empty(&s.rx[0]) && !s.download->image_complete);
    mdm_sahara_receive(&s, image + 43, 5);
    assert(s.download->image_complete &&
           !memcmp(s.download->image->data, image, 48));
    assert(mdm_read(&s, 0, out, sizeof(out)) == 16);
    assert(ldl_le_p(out) == 4 && ldl_le_p(out + 8) == 22 &&
           !ldl_le_p(out + 12));
    uint8_t done[8] = {5, 0, 0, 0, 8};
    mdm_sahara_receive(&s, done, sizeof(done));
    assert(mdm_read(&s, 0, out, sizeof(out)) == 12);
    assert(ldl_le_p(out) == 6 && !ldl_le_p(out + 8));
    assert(mdm_read(&s, 0, out, sizeof(out)) == 48 && ldl_le_p(out) == 1);
    assert(s.download->image_index == 1 && !s.download->image &&
           !s.download->image_complete);
    mdm_sahara_receive(&s, done, sizeof(done));
    assert(g_queue_is_empty(&s.rx[0])); /* duplicate DONE cannot advance */
    mdm_download_free(s.download);
    s.download = g_new0(MdmDownloadState, 1);
    mdm_sahara_receive(&s, hello_response, sizeof(hello_response));
    mdm_read(&s, 0, out, sizeof(out));
    stl_le_p(image + 16, 0xffffffff);
    mdm_sahara_receive(&s, image, 40);
    assert(mdm_read(&s, 0, out, sizeof(out)) == 16);
    assert(ldl_le_p(out) == 4 && ldl_le_p(out + 12) == 7);
    assert(!s.download->image_complete && !s.download->image_pending);
    mdm_download_free(s.download);
    s.download = g_new0(MdmDownloadState, 1);
    s.download->image_index = 2;
    mdm_sahara_receive(&s, hello_response, sizeof(hello_response));
    mdm_read(&s, 0, out, sizeof(out));
    uint8_t elf[88] = {0x7f, 'E', 'L', 'F', 1, 1, 1};
    elf[16] = 2;
    elf[18] = 164;
    elf[20] = 1;
    stl_le_p(elf + 24, 0x40200000);
    stl_le_p(elf + 28, 52);
    elf[40] = 52;
    elf[42] = 32;
    elf[44] = 1;
    stl_le_p(elf + 52, 1);
    stl_le_p(elf + 56, 84);
    stl_le_p(elf + 60, 0x40200000);
    stl_le_p(elf + 64, 0x40200000);
    stl_le_p(elf + 68, 4);
    stl_le_p(elf + 72, 8);
    memcpy(elf + 84, "DSP!", 4);
    mdm_sahara_receive(&s, elf, 40);
    assert(mdm_read(&s, 0, out, sizeof(out)) == 20 && ldl_le_p(out + 16) == 12);
    mdm_sahara_receive(&s, elf + 40, 12);
    assert(mdm_read(&s, 0, out, sizeof(out)) == 20 && ldl_le_p(out + 16) == 32);
    assert(!s.download->image_size_known);
    mdm_sahara_receive(&s, elf + 52, 32);
    assert(s.download->image_size_known && s.download->image_total == 88);
    assert(mdm_read(&s, 0, out, sizeof(out)) == 20 && ldl_le_p(out + 16) == 4);
    mdm_sahara_receive(&s, elf + 84, 4);
    assert(s.download->image_complete &&
           !memcmp(s.download->image->data, elf, sizeof(elf)));
    assert(mdm_read(&s, 0, out, sizeof(out)) == 16 && !ldl_le_p(out + 12));
    assert(!mdm_sahara_finish_image(s.download) && s.download->dsp_count == 1);
    assert(!mdm_sahara_finish_image(s.download) && s.download->dsp_count == 1);
    stl_le_p(s.download->image->data + 24, 0x41800000);
    assert(!mdm_sahara_finish_image(s.download) && s.download->dsp_count == 2);
    stl_le_p(s.download->image->data + 24, 0x46f00000);
    assert(!mdm_sahara_finish_image(s.download) && s.download->dsp_count == 3);
    s.download->image->data[18] = 40;
    assert(
        !mdm_sahara_finish_image(s.download)); /* APPS alone is insufficient */
    s.download->image_elf = false;
    stl_le_p(s.download->image->data, 23);
    assert(!mdm_sahara_finish_image(s.download));
    stl_le_p(s.download->image->data, 22);
    assert(mdm_sahara_finish_image(s.download));
    assert(!radio_starts); /* Boot ROM and partial firmware cannot search. */
    test_clock = 123456;
    mdm_sahara_receive(&s, done, sizeof(done));
    assert(mdm_read(&s, 0, out, sizeof(out)) == 12 && ldl_le_p(out + 8) == 1);
    assert(s.download->runtime && g_queue_is_empty(&s.rx[0]));
    assert(radio_starts == 1 && radio_started_at == test_clock);
    mdm_sahara_receive(&s, done, sizeof(done));
    assert(radio_starts == 1); /* Duplicate DONE must not restart searching. */
    mdm_download_free(s.download);
    s.download = g_new0(MdmDownloadState, 1);
    mdm_sahara_receive(&s, hello_response, sizeof(hello_response));
    mdm_read(&s, 0, out, sizeof(out));
    elf[44] = 0xff;
    elf[45] = 0xff;
    mdm_sahara_receive(&s, elf, 40);
    mdm_read(&s, 0, out, sizeof(out));
    mdm_sahara_receive(&s, elf + 40, 12);
    assert(mdm_read(&s, 0, out, sizeof(out)) == 16 && ldl_le_p(out + 12) == 7);
    assert(!s.download->image_complete && s.download->image->len == 52);
    mdm_download_free(s.download);
    for (unsigned i = 0; i < 13; i++) {
        g_queue_clear_full(&s.rx[i], (GDestroyNotify)g_bytes_unref);
        g_byte_array_unref(s.tx[i]);
    }
    puts(
        "PASS: native DLOAD parameters, split/escaped HDLC, CRC, bounded recovery, SRAM writes/address checks and QMI isolation");
}
