#include "hw/arm/ipod_touch_nor_spi.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/log.h"

/*
 * The NOR carries an nvram partition made of 16-byte atoms:
 *
 *     uint8_t  tag;
 *     uint8_t  checksum;   sum of the payload bytes, plus one
 *     uint16_t size;       payload size in 16-byte units
 *     char     name[12];
 *
 * The atom named "common" holds iBoot's environment as NUL-terminated
 * "key=value" strings, terminated by an empty string. boot-args lives there,
 * and is how the kernel gets io=0xffff and friends.
 */
#define NVRAM_ATOM_HDR   16
#define NVRAM_ATOM_UNIT  16

static uint8_t nvram_checksum(const uint8_t *payload, size_t len)
{
    uint8_t sum = 0;
    for (size_t i = 0; i < len; i++) {
        sum += payload[i];
    }
    return sum + 1;
}

/*
 * Rewrite the boot-args variable in the in-memory copy of the NOR. The image
 * on disk is never touched.
 *
 * BROKEN, do not rely on this yet. On the stock n72ap NOR the atom chain is
 *
 *   0x0fc000  "nvram"          size 2 units    (32 bytes)
 *   0x0fc020  "common"         size 128 units  (2048 bytes)
 *   0x0fc820  "APL,OSXPanic"                   <- inside common's declared span
 *
 * so the memset below, which clears all 2048 bytes the "common" header claims,
 * destroys the "APL,OSXPanic" atom that follows it. A machine started with
 * boot-args= then never reaches its serial banner: it sits in LLB with no
 * output, where the same NOR untouched gets to "Loading kernel cache".
 *
 * Fix by clamping the rewrite to the bytes actually used by the variable list
 * rather than the declared atom size, or by working out why "common" declares
 * a span that swallows its successor.
 */
static void nor_set_boot_args(IPodTouchNORSPIState *s, gsize norlen)
{
    static const char key[] = "boot-args=";

    for (gsize off = 0; off + NVRAM_ATOM_HDR <= norlen; off += NVRAM_ATOM_UNIT) {
        uint8_t *hdr = s->nor_data + off;
        if (memcmp(hdr + 4, "common\0", 7) != 0) {
            continue;
        }

        size_t len = (size_t)lduw_le_p(hdr + 2) * NVRAM_ATOM_UNIT;
        if (len == 0 || off + NVRAM_ATOM_HDR + len > norlen) {
            break;
        }
        uint8_t *payload = hdr + NVRAM_ATOM_HDR;

        /* Copy out every variable except the one we are replacing, and note
         * where the original NUL-terminated list ends. */
        g_autoptr(GByteArray) vars = g_byte_array_new();
        size_t list_end = 0;
        for (size_t i = 0; i < len && payload[i]; ) {
            size_t vlen = strnlen((char *)payload + i, len - i);
            if (strncmp((char *)payload + i, key, strlen(key)) != 0) {
                g_byte_array_append(vars, payload + i, vlen + 1);
            }
            i += vlen + 1;
            list_end = i;
        }

        g_autofree char *entry = g_strconcat(key, s->boot_args, NULL);
        g_byte_array_append(vars, (const guint8 *)entry, strlen(entry) + 1);
        g_byte_array_append(vars, (const guint8 *)"", 1); /* list terminator */

        /*
         * The "common" atom declares 2048 bytes, but the following
         * "APL,OSXPanic" atom physically sits INSIDE that declared span (e.g.
         * at payload offset ~2040 on n72ap). Clearing the whole declared span
         * corrupts OSXPanic and makes iBoot heap-panic on boot. So only rewrite
         * the region the variable list actually uses -- the original entries
         * plus the free (zero) padding that follows -- and never touch bytes
         * from the next atom onward. Find that boundary by walking past the
         * trailing zero padding to the first non-zero byte (the next atom).
         */
        size_t write_limit = list_end;
        while (write_limit < len && payload[write_limit] == 0) {
            write_limit++;
        }

        if (vars->len > write_limit) {
            error_report("boot-args does not fit before the next nvram atom "
                         "(%u bytes needed, %zu available)", vars->len,
                         write_limit);
            return;
        }

        memset(payload, 0, write_limit);
        memcpy(payload, vars->data, vars->len);
        hdr[1] = nvram_checksum(payload, len);

        printf("[NVRAM] boot-args=%s\n", s->boot_args);
        return;
    }

    error_report("could not find the nvram \"common\" atom in the NOR image; "
                 "boot-args not applied");
}

static void initialize_nor(IPodTouchNORSPIState *s)
{
    gsize fsize = 0;
    GError *err = NULL;

    if (g_file_get_contents(s->nor_path, (char **)&s->nor_data, &fsize, &err)) {
        s->nor_size = fsize;
        s->nor_initialized = true;
        if (s->boot_args && s->boot_args[0]) {
            nor_set_boot_args(s, fsize);
        }
        return;
    }

    /*
     * This used to fail silently, leaving nor_data NULL for a read path that
     * dereferences it unconditionally. Say so instead -- an unreadable NOR is
     * a setup mistake, and the alternative is a segfault with no explanation.
     */
    error_report("NOR image \"%s\" could not be read: %s. NOR reads will "
                 "return 0xff.", s->nor_path, err ? err->message : "unknown");
    g_clear_error(&err);
    s->nor_data = NULL;
    s->nor_size = 0;
    s->nor_initialized = true;      /* do not retry on every transfer */
}

static uint32_t ipod_touch_nor_spi_transfer(SSIPeripheral *dev, uint32_t value)
{
    IPodTouchNORSPIState *s = IPOD_TOUCH_NOR_SPI(dev);

    if(s->cur_cmd == NOR_READ_DATA_CMD && s->in_buf_cur_ind == s->in_buf_size && value != 0xFF) {
        // if we are currently reading from the NOR data and we receive a value that's not the sentinel, reset the current command.
        s->cur_cmd = 0;
    }

    if(s->cur_cmd == 0) {
        // this is a new command -> set it
        s->cur_cmd = value;
        s->out_buf = malloc(0x1000);
        s->in_buf = malloc(0x1000);
        s->in_buf[0] = value;
        s->in_buf_size = 0;
        s->in_buf_cur_ind = 1;
        s->out_buf_cur_ind = 0;

        if(value == NOR_WRITE_TO_STATUS_REG) {
            s->in_buf_size = 1;
            s->out_buf_size = 1;
            s->out_buf[0] = 0;
        }
        else if(value == NOR_GET_STATUS_CMD) {
            s->in_buf_size = 1;
            s->out_buf_size = 1;
            s->out_buf[0] = 0;
        }
        else if(value == NOR_WRITE_DATA_CMD) {
            s->in_buf_size = 4;
            s->out_buf_size = 256;
            for(int i = 0; i < 256; i++) { s->out_buf[i] = 0; }; // TODO we ignore this command for now
        }
        else if(value == NOR_READ_DATA_CMD) {
            s->in_buf_size = 4;
            s->out_buf_size = 4096;
        }
        else if(value == NOR_ERASE_BLOCK) {
            s->in_buf_size = 1;
            s->out_buf_size = 3;
            for(int i = 0; i < 3; i++) { s->out_buf[i] = 0x0; } // TODO we ignore this command for now
        }
        else if(value == NOR_ENABLE_WRITE) {
            s->write_enabled = 1;
            s->cur_cmd = 0;
        }
        else if(value == NOR_DISABLE_WRITE) {
            s->write_enabled = 0;
            s->cur_cmd = 0;
        }
        else if(value == NOR_GET_JEDECID) {
            s->in_buf_size = 1;
            s->out_buf_size = 3;
            s->out_buf[0] = 0x1F; // vendor: atmel, device: 0x02 -> AT25DF081A
            s->out_buf[1] = 0x45;
            s->out_buf[2] = 0x02;
        }
        else {
            /*
             * Nothing set in_buf_size/out_buf_size here, so the command ran on
             * the *previous* command's lengths -- in bounds only because both
             * buffers are 0x1000 and the largest size is 0x1000. Give it a
             * defined one-byte answer so the transaction terminates and the
             * buffers are freed on the completion path below.
             */
            printf("%s Unknown command 0x%02x!\n", __func__, value);
            s->in_buf_size = 1;
            s->out_buf_size = 1;
            s->out_buf[0] = 0;
        }

        return 0x0;
    }
    else if(s->cur_cmd != 0 && s->in_buf_cur_ind < s->in_buf_size) {
        // we're reading the command
        s->in_buf[s->in_buf_cur_ind] = value;
        s->in_buf_cur_ind++;

        if(s->cur_cmd == NOR_GET_STATUS_CMD && s->in_buf_cur_ind == s->in_buf_size) {
            s->out_buf[0] = 0x0;  // indicates that the NOR is ready
        }
        else if(s->cur_cmd == NOR_READ_DATA_CMD && s->in_buf_cur_ind == s->in_buf_size) {
            if(!s->nor_initialized) { initialize_nor(s); }
            s->nor_read_ind = (s->in_buf[1] << 16) | (s->in_buf[2] << 8) | s->in_buf[3];
        }
        return 0x0;
    }
    else {
        uint8_t ret_val;
        // otherwise, we're outputting the response
        if(s->cur_cmd == NOR_READ_DATA_CMD) {
            /*
             * nor_read_ind is a guest-supplied 24-bit address and the read is
             * a streaming one with no terminator, so it walks forward for as
             * long as the guest keeps clocking. Without a length it indexed a
             * heap buffer unbounded, and nor_data could be NULL outright if
             * the image failed to load. 0xff is what an absent or erased NOR
             * reads as, so it is the honest answer past the end.
             */
            uint8_t ret_val = 0xff;
            if (s->nor_data && s->nor_read_ind < s->nor_size) {
                ret_val = s->nor_data[s->nor_read_ind];
            } else {
                qemu_log_mask(LOG_GUEST_ERROR,
                              "[NOR] read past end of image (offset 0x%x, "
                              "size 0x%zx)\n", s->nor_read_ind,
                              (size_t)s->nor_size);
            }
            s->nor_read_ind++;
            return ret_val;
        }
        else {
            ret_val = s->out_buf[s->out_buf_cur_ind];
            s->out_buf_cur_ind++;

            if(s->cur_cmd != 0 && (s->out_buf_cur_ind == s->out_buf_size)) {
                // the command is done - clean up
                s->cur_cmd = 0;
                free(s->in_buf);
                free(s->out_buf);
            }
        }
        return ret_val;
    }
}

static void ipod_touch_nor_spi_realize(SSIPeripheral *d, Error **errp)
{
    IPodTouchNORSPIState *s = IPOD_TOUCH_NOR_SPI(d);
    s->nor_initialized = 0;
}

/*
 * nor_data is the NOR image, loaded from the same file by the destination's own
 * realize, so it is identical without being migrated -- but that also means a
 * guest WRITE to NOR made before the snapshot is lost on restore, since the
 * model keeps it only in that RAM copy. in_buf/out_buf hold a transaction in
 * flight; post_load rewinds to a clean command boundary rather than restoring
 * half a transfer.
 */
static int ipod_touch_nor_spi_post_load(void *opaque, int version_id)
{
    IPodTouchNORSPIState *s = opaque;

    s->cur_cmd = 0;
    s->in_buf_cur_ind = 0;
    s->out_buf_cur_ind = 0;
    s->in_buf_size = 0;
    s->out_buf_size = 0;
    return 0;
}

static const VMStateDescription vmstate_ipod_touch_nor_spi = {
    .name = "ipod_touch_nor_spi",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ipod_touch_nor_spi_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_SSI_PERIPHERAL(ssidev, IPodTouchNORSPIState),
        VMSTATE_UINT8(write_enabled, IPodTouchNORSPIState),
        VMSTATE_UINT32(nor_read_ind, IPodTouchNORSPIState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_nor_spi_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_nor_spi;
    SSIPeripheralClass *k = SSI_PERIPHERAL_CLASS(klass);
    k->realize = ipod_touch_nor_spi_realize;
    k->transfer = ipod_touch_nor_spi_transfer;
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
