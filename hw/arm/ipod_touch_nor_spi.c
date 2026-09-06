#include "hw/arm/ipod_touch_nor_spi.h"
#include "migration/vmstate.h"
#include "trace.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"

#include <zlib.h>

/* CHRP partition lengths include their 16-byte header. Its checksum covers
 * the header only; the containing Apple 8 KiB bank has a separate Adler-32.
 * See IONVRAM and iBoot's nvram_load/load_bank_partitions. */
#define NVRAM_BANK_SIZE 0x2000
#define NVRAM_ATOM_HDR 16

static uint8_t nvram_checksum(const uint8_t *hdr)
{
    unsigned sum = hdr[0];
    for (unsigned i = 2; i < NVRAM_ATOM_HDR; i++) sum += hdr[i];
    while (sum > 255) sum = (sum & 255) + (sum >> 8);
    return sum;
}

static void nor_set_boot_args(IPodTouchNORSPIState *s, gsize norlen)
{
    static const char key[] = "boot-args=";
    for (gsize off = 0; off + NVRAM_BANK_SIZE <= norlen; off += NVRAM_BANK_SIZE) {
        uint8_t *bank = s->nor_data + off;
        if (bank[0] != 0x5a || memcmp(bank + 4, "nvram\0", 6) ||
            lduw_le_p(bank + 2) != 2 || bank[1] != nvram_checksum(bank) ||
            ldl_le_p(bank + 16) != adler32(1, bank + 20, NVRAM_BANK_SIZE - 20)) {
            continue;
        }
        for (size_t pos = 32; pos + NVRAM_ATOM_HDR <= NVRAM_BANK_SIZE;) {
            uint8_t *hdr = bank + pos;
            size_t total = (size_t)lduw_le_p(hdr + 2) * 16;
            if (hdr[1] != nvram_checksum(hdr) || total < NVRAM_ATOM_HDR ||
                total > NVRAM_BANK_SIZE - pos || hdr[0] == 0x7f) break;
            pos += total;
            if (memcmp(hdr + 4, "common\0", 7)) continue;

            uint8_t *payload = hdr + NVRAM_ATOM_HDR;
            size_t len = total - NVRAM_ATOM_HDR, i = 0;
            g_autoptr(GByteArray) vars = g_byte_array_new();
            while (i < len && payload[i]) {
                size_t n = strnlen((char *)payload + i, len - i);
                if (n == len - i) break;
                if (n < sizeof(key) - 1 || memcmp(payload + i, key, sizeof(key) - 1)) {
                    g_byte_array_append(vars, payload + i, n + 1);
                }
                i += n + 1;
            }
            if (i == len || payload[i]) {
                error_report("malformed common NVRAM variable list");
                break;
            }
            g_autofree char *entry = g_strconcat(key, s->boot_args, NULL);
            g_byte_array_append(vars, (const uint8_t *)entry, strlen(entry) + 1);
            g_byte_array_append(vars, (const uint8_t *)"", 1);
            if (vars->len > len) {
                error_report("boot-args does not fit in common NVRAM partition");
                break;
            }
            memset(payload, 0, len);
            memcpy(payload, vars->data, vars->len);
            stl_le_p(bank + 16, adler32(1, bank + 20, NVRAM_BANK_SIZE - 20));
            printf("[NVRAM] boot-args=%s\n", s->boot_args);
            break;
        }
    }
}


static void initialize_nor(IPodTouchNORSPIState *s)
{
    gsize size = 0;
    GError *error = NULL;
    g_autofree char *data = NULL;

    memset(s->nor_data, 0xff, sizeof(s->nor_data));
    s->nor_size = 0;
    s->nor_initialized = true;
    if (!g_file_get_contents(s->nor_path, &data, &size, &error) ||
        size != NOR_FLASH_SIZE) {
        error_report("NOR image \"%s\" must contain exactly %u bytes: %s; reads return 0xff",
                     s->nor_path, NOR_FLASH_SIZE,
                     error ? error->message : "incorrect image size");
        g_clear_error(&error);
        return;
    }
    memcpy(s->nor_data, data, size);
    s->nor_size = size;
    if (s->boot_args && s->boot_args[0]) {
        nor_set_boot_args(s, size);
    }
}

#define NOR_STATUS_EPE 0x20
#define NOR_STATUS_SWP 0x0c
#define NOR_STATUS_SPRL 0x80

static void nor_reset_transaction(IPodTouchNORSPIState *s)
{
    s->cur_cmd = 0;
    s->command_active = false;
    s->address_bytes = 0;
    s->data_count = 0;
    s->stream_offset = 0;
    s->page_offset = 0;
}

static void nor_finish_transaction(IPodTouchNORSPIState *s)
{
    trace_ipod_touch_nor_finish(s->cur_cmd, s->nor_read_ind, s->address_bytes,
                               s->data_count, s->write_enabled, s->status, s->page[0]);
    if (!s->command_active) {
        return;
    }
    if (s->cur_cmd == NOR_ENABLE_WRITE) {
        s->write_enabled = 1;
        return;
    }
    if (s->cur_cmd == NOR_DISABLE_WRITE) {
        s->write_enabled = 0;
        return;
    }
    if (!s->write_enabled) {
        return;
    }
    if (s->cur_cmd == NOR_WRITE_TO_STATUS_REG) {
        if (s->data_count) {
            uint8_t value = s->page[0];
            /* WP is deasserted. A locked SPRL must be cleared by a separate
             * command before changing the global sector protection. */
            if (!(s->status & NOR_STATUS_SPRL)) {
                if ((value & 0x3c) == 0) {
                    s->status &= ~NOR_STATUS_SWP;
                } else if ((value & 0x3c) == 0x3c) {
                    s->status |= NOR_STATUS_SWP;
                }
            }
            s->status = (s->status & ~NOR_STATUS_SPRL) | (value & NOR_STATUS_SPRL);
        }
        s->write_enabled = 0;
        return;
    }
    uint32_t length;
    switch (s->cur_cmd) {
    case NOR_WRITE_DATA_CMD: length = NOR_PAGE_SIZE; break;
    case NOR_ERASE_BLOCK: length = 4096; break;
    case NOR_ERASE_32K: length = 32768; break;
    case NOR_ERASE_64K: length = 65536; break;
    default: return;
    }
    s->write_enabled = 0;
    if (s->address_bytes != 3 ||
        (s->cur_cmd == NOR_WRITE_DATA_CMD && !s->data_count) ||
        (s->status & NOR_STATUS_SWP)) {
        return;
    }
    s->status &= ~NOR_STATUS_EPE;
    if (!s->nor_initialized) {
        initialize_nor(s);
    }
    if (s->nor_size != NOR_FLASH_SIZE) {
        s->status |= NOR_STATUS_EPE;
        return;
    }
    uint32_t start = s->nor_read_ind & ~(length - 1);
    /* ponytail: operations complete synchronously at CS release; add a virtual
     * busy timer when a guest requires programming/erase latency. */
    if (s->cur_cmd == NOR_WRITE_DATA_CMD) {
        for (unsigned i = 0; i < NOR_PAGE_SIZE; i++) {
            s->nor_data[start + i] &= s->page[i];
        }
    } else {
        memset(s->nor_data + start, 0xff, length);
    }
}

static int ipod_touch_nor_spi_set_cs(SSIPeripheral *dev, bool level)
{
    IPodTouchNORSPIState *s = IPOD_TOUCH_NOR_SPI(dev);
    if (level) {
        nor_finish_transaction(s);
    }
    nor_reset_transaction(s);
    return 0;
}

static void ipod_touch_nor_spi_reset(DeviceState *dev)
{
    IPodTouchNORSPIState *s = IPOD_TOUCH_NOR_SPI(dev);
    nor_reset_transaction(s);
    s->write_enabled = 0;
    s->status = NOR_STATUS_SWP;
}

static uint32_t ipod_touch_nor_spi_transfer(SSIPeripheral *dev, uint32_t value)
{
    IPodTouchNORSPIState *s = IPOD_TOUCH_NOR_SPI(dev);
    value &= 0xff;
    if (!s->command_active) {
        trace_ipod_touch_nor_command(value);
        s->command_active = true;
        s->cur_cmd = value;
        s->nor_read_ind = 0;
        if (value == NOR_WRITE_DATA_CMD) {
            memset(s->page, 0xff, sizeof(s->page));
        }
        return 0;
    }
    switch (s->cur_cmd) {
    case NOR_READ_DATA_CMD:
    case NOR_WRITE_DATA_CMD:
    case NOR_ERASE_BLOCK:
    case NOR_ERASE_32K:
    case NOR_ERASE_64K:
        if (s->address_bytes < 3) {
            s->nor_read_ind = ((s->nor_read_ind << 8) | value) & (NOR_FLASH_SIZE - 1);
            if (++s->address_bytes == 3) {
                s->page_offset = s->nor_read_ind & (NOR_PAGE_SIZE - 1);
            }
            return 0;
        }
        if (s->cur_cmd == NOR_READ_DATA_CMD) {
            if (!s->nor_initialized) {
                initialize_nor(s);
            }
            uint8_t result = s->nor_data[s->nor_read_ind];
            s->nor_read_ind = (s->nor_read_ind + 1) & (NOR_FLASH_SIZE - 1);
            return result;
        }
        if (s->cur_cmd == NOR_WRITE_DATA_CMD) {
            /* The last byte clocked into each page-buffer position wins; only
             * CS release ANDs that final buffer into the flash array. */
            s->page[s->page_offset++] = value;
            if (s->data_count < NOR_PAGE_SIZE) {
                s->data_count++;
            }
        }
        return 0;
    case NOR_WRITE_TO_STATUS_REG:
        if (!s->data_count) {
            s->page[0] = value;
            s->data_count = 1;
        }
        return 0;
    case NOR_GET_STATUS_CMD:
        s->stream_offset ^= 1;
        return s->stream_offset ? s->status | 0x10 | (s->write_enabled ? 2 : 0) : 0;
    case NOR_GET_JEDECID: {
        static const uint8_t id[] = {0x1f, 0x45, 0x02, 0x00};
        return s->stream_offset < sizeof(id) ? id[s->stream_offset++] : 0xff;
    }
    default:
        return 0xff;
    }
}

static void ipod_touch_nor_spi_realize(SSIPeripheral *d, Error **errp)
{
    IPodTouchNORSPIState *s = IPOD_TOUCH_NOR_SPI(d);
    s->nor_initialized = false;
}

static int ipod_touch_nor_spi_pre_save(void *opaque)
{
    IPodTouchNORSPIState *s = opaque;
    if (!s->nor_initialized) {
        initialize_nor(s);
    }
    return 0;
}

static int ipod_touch_nor_spi_post_load(void *opaque, int version_id)
{
    IPodTouchNORSPIState *s = opaque;
    if (version_id < 2) {
        initialize_nor(s);
        nor_reset_transaction(s);
        s->status = NOR_STATUS_SWP;
        s->nor_read_ind = 0;
    }
    if (!s->nor_initialized || (s->status & ~(NOR_STATUS_SPRL | NOR_STATUS_SWP | NOR_STATUS_EPE)) ||
        s->nor_read_ind >= NOR_FLASH_SIZE || s->cur_cmd > 0xff ||
        s->address_bytes > 3 || s->data_count > NOR_PAGE_SIZE ||
        s->stream_offset > 4 || s->write_enabled > 1 ||
        (s->nor_size != 0 && s->nor_size != NOR_FLASH_SIZE)) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_ipod_touch_nor_spi = {
    .name = "ipod_touch_nor_spi",
    .version_id = 2,
    .minimum_version_id = 1,
    .pre_save = ipod_touch_nor_spi_pre_save,
    .post_load = ipod_touch_nor_spi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(ssidev, IPodTouchNORSPIState),
        VMSTATE_UINT8(write_enabled, IPodTouchNORSPIState),
        VMSTATE_UINT32(nor_read_ind, IPodTouchNORSPIState),
        VMSTATE_UINT32_V(cur_cmd, IPodTouchNORSPIState, 2),
        VMSTATE_UINT32_V(nor_size, IPodTouchNORSPIState, 2),
        VMSTATE_UINT8_V(address_bytes, IPodTouchNORSPIState, 2),
        VMSTATE_UINT8_V(page_offset, IPodTouchNORSPIState, 2),
        VMSTATE_UINT8_V(stream_offset, IPodTouchNORSPIState, 2),
        VMSTATE_UINT8_V(status, IPodTouchNORSPIState, 2),
        VMSTATE_UINT16_V(data_count, IPodTouchNORSPIState, 2),
        VMSTATE_BOOL_V(command_active, IPodTouchNORSPIState, 2),
        VMSTATE_BOOL_V(nor_initialized, IPodTouchNORSPIState, 2),
        VMSTATE_BUFFER_V(page, IPodTouchNORSPIState, 2),
        VMSTATE_BUFFER_V(nor_data, IPodTouchNORSPIState, 2),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_nor_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_nor_spi;
    device_class_set_legacy_reset(dc, ipod_touch_nor_spi_reset);
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    k->realize = ipod_touch_nor_spi_realize;
    k->transfer = ipod_touch_nor_spi_transfer;
    k->set_cs = ipod_touch_nor_spi_set_cs;
    k->cs_polarity = SSI_CS_LOW;
}

static const TypeInfo ipod_touch_nor_spi_type_info = {
    .name = TYPE_IPOD_TOUCH_NOR_SPI,
    .parent = TYPE_SSI_PERIPHERAL,
    .instance_size = sizeof(IPodTouchNORSPIState),
    .class_init = ipod_touch_nor_spi_class_init,
};

static void ipod_touch_nor_spi_register_types(void)
{
    type_register_static(&ipod_touch_nor_spi_type_info);
}

type_init(ipod_touch_nor_spi_register_types)
