/*
 * Imagination Topaz HP register interface used by the A6 VXE380 provider.
 * The virtual device advertises one pipe and the 3.3 register interface;
 * those identify this implementation, rather than a measured chip revision.
 * Codec command execution is still being brought up.
 */
#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "migration/vmstate.h"
#include "system/address-spaces.h"
#include "img_topaz_h264.h"
#include "trace.h"

#define TYPE_IMG_TOPAZ "img.topaz-hp"
#define TOPAZ_SIZE 0x100000
#define TOPAZ_MTX_BASE 0x82880000
#define TOPAZ_MTX_SIZE 0x20000
OBJECT_DECLARE_SIMPLE_TYPE(IMGTopazState, IMG_TOPAZ)
struct IMGTopazState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq irq;
    uint32_t regs[TOPAZ_SIZE / 4];
    uint8_t mtx_ram[TOPAZ_MTX_SIZE];
    uint32_t mtx_registers[0x10000];
    uint32_t commands[64];
    uint32_t command_count;
    uint32_t video_context, width, height;
    uint32_t coded[4][3];   /* Address, capacity, available. */
    uint32_t sources[8][3]; /* Y, UV, source index. */
    uint32_t source_count, qp, frame_count;
    uint32_t reconstruction;
    TopazH264 *encoder;
    bool force_idr;
    bool context_saved;
    bool firmware_loaded, firmware_started;
};

static bool topaz_dma(IMGTopazState *s, uint32_t va, uint8_t *dst,
                      size_t length, bool write)
{
    if (!length || (uint64_t)va + length > UINT64_C(0x100000000)) {
        return false;
    }
    uint32_t root = s->regs[0x30 / 4] & ~0xfffu;
    while (length) {
        MemTxResult result;
        uint32_t pde =
            address_space_ldl_le(&address_space_memory, root + (va >> 22) * 4,
                                 MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK || !(pde & 1))
            return false;
        uint32_t pte = address_space_ldl_le(
            &address_space_memory, (pde & ~0xfffu) + ((va >> 12) & 0x3ff) * 4,
            MEMTXATTRS_UNSPECIFIED, &result);
        if (result != MEMTX_OK || !(pte & 1))
            return false;
        size_t n = MIN(length, 0x1000 - (va & 0xfff));
        hwaddr pa = (pte & ~0xfffu) | (va & 0xfff);
        result = write ? address_space_write(&address_space_memory, pa,
                                             MEMTXATTRS_UNSPECIFIED, dst, n)
                       : address_space_read(&address_space_memory, pa,
                                            MEMTXATTRS_UNSPECIFIED, dst, n);
        if (result != MEMTX_OK)
            return false;
        va += n;
        dst += n;
        length -= n;
    }
    return true;
}
static bool topaz_mtx_dma(IMGTopazState *s, uint32_t control)
{
    uint32_t target = s->regs[0xb44 / 4];
    size_t length = (control & 0xffff) * 4;
    if (target < TOPAZ_MTX_BASE ||
        (uint64_t)target + length > TOPAZ_MTX_BASE + TOPAZ_MTX_SIZE) {
        return false;
    }
    return topaz_dma(s, s->regs[0x400 / 4],
                     s->mtx_ram + (target - TOPAZ_MTX_BASE), length,
                     control & 0x4000000);
}
static void topaz_irq(IMGTopazState *s)
{
    bool pending = (s->regs[4 / 4] & s->regs[0xc / 4] & 0x7fffffff) != 0;
    qemu_set_irq(s->irq, pending && (s->regs[0xc / 4] & 0x80000000));
}
static unsigned topaz_response_space(IMGTopazState *s)
{
    return (s->regs[0x108 / 4] - s->regs[0x104 / 4] - 1) & 31;
}
static bool topaz_response(IMGTopazState *s, uint32_t context, uint32_t type,
                           uint32_t token, uint32_t value)
{
    unsigned producer = s->regs[0x104 / 4] & 31;
    uint8_t pointer[4], response[16] = {0};
    if (!topaz_response_space(s) || !topaz_dma(s, context + 0xc4 + producer * 4,
                                               pointer, sizeof(pointer), false))
        return false;
    stl_le_p(response, type);
    stl_le_p(response + 8, token);
    stl_le_p(response + 12, value);
    if (!topaz_dma(s, ldl_le_p(pointer), response, sizeof(response), true))
        return false;
    s->regs[0x104 / 4] = (s->regs[0x104 / 4] & ~31u) | ((producer + 1) & 31);
    s->regs[4 / 4] |= 2;
    topaz_irq(s);
    return true;
}
/* Reference storage is tiled: 16-byte columns, 32 luma rows or 16
 * interleaved-chroma rows per tile. Pad the bottom/right with edge pixels. */
static void topaz_tile_picture(uint8_t *dst, const uint8_t *src, unsigned width,
                               unsigned height)
{
    unsigned pitch = ROUND_UP(width, 16), rows = ROUND_UP(height, 32);
    for (unsigned plane = 0; plane < 2; plane++) {
        unsigned tile_h = 32 >> plane, h = height >> plane;
        for (unsigned y = 0; y < (rows >> plane); y++) {
            for (unsigned x = 0; x < pitch; x++) {
                size_t at = (size_t)(y / tile_h) * pitch * tile_h +
                            (x / 16) * 16 * tile_h + (y % tile_h) * 16 + x % 16;
                unsigned col = plane && x >= width ? width - 2 + (x & 1)
                                                   : MIN(x, width - 1);
                dst[at] = src[(size_t)MIN(y, h - 1) * width + col];
            }
        }
        if (!plane) {
            src += (size_t)width * height;
            dst += (size_t)pitch * rows;
        }
    }
}
static bool topaz_encode(IMGTopazState *s, uint32_t ticket)
{
    unsigned slot;
    for (slot = 0; slot < 4 && !s->coded[slot][2]; slot++) {
    }
    if (!s->source_count || slot == 4 || topaz_response_space(s) < 2)
        return false;
    size_t ysize = (size_t)s->width * s->height;
    g_autofree uint8_t *pixels = g_malloc(ysize + ysize / 2);
    if (!topaz_dma(s, s->sources[0][0], pixels, ysize, false) ||
        !topaz_dma(s, s->sources[0][1], pixels + ysize, ysize / 2, false))
        return false;
    trace_a6_vxe_source(s->frame_count, s->force_idr, s->sources[0][0],
                        s->sources[0][1], ldl_le_p(pixels),
                        ldl_le_p(pixels + ysize));
    if (!s->encoder)
        s->encoder = topaz_h264_new(s->width, s->height);
    uint8_t *encoded = NULL;
    size_t length = 0;
    if (!topaz_h264_encode(s->encoder, pixels, pixels + ysize, s->width, s->qp,
                           s->force_idr || !s->frame_count, &encoded, &length))
        return false;
    g_autofree uint8_t *output = encoded;
    if (s->reconstruction) {
        size_t room =
            (size_t)ROUND_UP(s->width, 16) * ROUND_UP(s->height, 32) * 3 / 2;
        g_autofree uint8_t *tiled = g_malloc(room);
        if (!topaz_h264_picture(s->encoder, pixels, pixels + ysize, s->width))
            return false;
        topaz_tile_picture(tiled, pixels, s->width, s->height);
        if (!topaz_dma(s, s->reconstruction, tiled, room, true))
            return false;
    }
    uint32_t dst = s->coded[slot][0], capacity = s->coded[slot][1];
    if (length + 64 > capacity ||
        (uint64_t)dst + 64 + length > UINT64_C(0x100000000))
        return false;
    uint8_t header[64] = {0};
    stl_le_p(header, length);
    /* Firmware feedback: storage number [1:0], source slot [5:2],
     * complete picture [10], coded picture [11], picture type [15:14].
     * A byte count alone describes an uncoded picture to the native driver. */
    bool idr = false;
    for (size_t i = 0; i + 4 < length; i++) {
        if (!memcmp(output + i, "\0\0\0\1", 4) && (output[i + 4] & 31) == 5) {
            idr = true;
            break;
        }
    }
    stl_le_p(header + 4, (s->frame_count & 3) | ((s->sources[0][2] & 15) << 2) |
                             (1u << 10) | (1u << 11) | ((idr ? 0u : 2u) << 14));
    stl_le_p(header + 8, 1u << 16); /* One coded slice. */
    header[0x1a] = s->qp;
    if (!topaz_dma(s, dst, header, sizeof(header), true) ||
        !topaz_dma(s, dst + sizeof(header), output, length, true))
        return false;
    if (!topaz_response(s, s->video_context, 1, slot, 0) ||
        !topaz_response(s, s->video_context, 0, ticket, 0))
        return false;
    s->coded[slot][2] = false;
    s->source_count--;
    memmove(s->sources, s->sources + 1,
            s->source_count * sizeof(s->sources[0]));
    s->frame_count++;
    s->force_idr = false;
    s->reconstruction = 0;
    return true;
}
static void topaz_commands(IMGTopazState *s)
{
    while (s->command_count >= 4) {
        uint32_t *c = s->commands;
        trace_a6_vxe_command(c[0], c[1], c[2], c[3]);
        switch (c[0] & 0x7f) {
        case 4: { /* SETVIDEO */
            uint8_t context[12], reply[16] = {0};
            if (!topaz_dma(s, c[1], context, sizeof(context), false))
                return;
            uint32_t w = lduw_le_p(context + 8), h = lduw_le_p(context + 10);
            if (!w || !h || w > 4096 || h > 4096 || ((w | h) & 1))
                return;
            if (!topaz_response(s, c[1], 0, c[3], 0))
                return;
            stl_le_p(reply, c[0]);
            stl_le_p(reply + 4, c[3]);
            if (!topaz_dma(s, c[2], reply, sizeof(reply), true))
                return;
            if (s->width != w || s->height != h || s->video_context != c[1]) {
                topaz_h264_free(s->encoder);
                s->encoder = NULL;
                s->frame_count = 0;
            }
            s->video_context = c[1];
            s->context_saved = false;
            s->width = w;
            s->height = h;
            s->qp = 24;
            break;
        }
        case 5: { /* GETVIDEO: save the current encoding context. */
            uint8_t reply[16] = {0};
            if (!s->video_context || c[1] != s->video_context ||
                !topaz_response_space(s))
                return;
            /* Codec/DPB state belongs to this virtual device, rather than
             * executing MTX RAM. It remains live across the guest's context
             * save/restore; guest-owned context fields are left intact. */
            stl_le_p(reply, c[0]);
            stl_le_p(reply + 4, c[3]);
            if (!topaz_dma(s, c[2], reply, sizeof(reply), true) ||
                !topaz_response(s, c[1], 0, c[3], 0))
                return;
            s->context_saved = true;
            break;
        }
        case 6: { /* Picture management controls. */
            uint8_t control[8];
            if (!topaz_dma(s, c[1], control, sizeof(control), false))
                return;
            uint32_t kind = ldl_le_p(control), value = ldl_le_p(control + 4);
            trace_a6_vxe_control(kind, value);
            switch (kind) {
            case 0:
                s->force_idr = value == 3;
                break;
            case 4:
                if (value > 51)
                    return;
                s->qp = value;
                break;
            case 5: /* Flush through the supplied completed-frame count. */
                /* The low-delay backend emits each picture synchronously.
                 * There are no delayed B pictures to drain, but do not pass
                 * this barrier while sources or requested frames remain. */
                if (s->source_count)
                    return;
                break;
            case 7:
                break; /* Host encoder performs rate control. */
            default:
                return;
            }
            break;
        }
        case 7: { /* Provide source, reference or coded buffer. */
            uint8_t buffer[32];
            if (!topaz_dma(s, c[1], buffer, sizeof(buffer), false))
                return;
            unsigned kind = ldl_le_p(buffer);
            trace_a6_vxe_buffer(kind, ldl_le_p(buffer + 4),
                                ldl_le_p(buffer + 8), ldl_le_p(buffer + 12));
            if (kind == 4) {
                unsigned slot = ldl_le_p(buffer + 12) & 0xff;
                uint32_t capacity = ldl_le_p(buffer + 8);
                if (slot >= 4 || capacity < 64 || capacity > 32 * 1024 * 1024)
                    return;
                s->coded[slot][0] = ldl_le_p(buffer + 4);
                s->coded[slot][1] = capacity;
                s->coded[slot][2] = true;
            } else if (kind == 0) {
                if (s->source_count == G_N_ELEMENTS(s->sources))
                    return;
                uint32_t *source = s->sources[s->source_count++];
                source[0] = ldl_le_p(buffer + 8);
                source[1] = ldl_le_p(buffer + 12);
                source[2] = ldl_le_p(buffer + 4);
            } else if (kind == 1) {
                s->reconstruction = ldl_le_p(buffer + 4);
            } else if (kind != 3) {
                return;
            } /* Reference pictures are held by the host codec's DPB. */
            break;
        }
        case 3: /* ENCODE_FRAME */
            if (!topaz_encode(s, c[3]))
                return;
            break;
        default:
            return;
        }
        s->command_count -= 4;
        memmove(s->commands, s->commands + 4,
                s->command_count * sizeof(s->commands[0]));
        topaz_irq(s);
    }
}
static uint64_t topaz_read(void *opaque, hwaddr off, unsigned size)
{
    IMGTopazState *s = opaque;
    switch (off) {
    case 4: {
        uint32_t value = s->regs[off / 4] & 0x7fffffff;
        return value | ((value & s->regs[0xc / 4]) ? 0x80000000u : 0);
    }
    case 0x44:
        /* MTX_DEBUG_MSTR: four 32-KiB RAM banks, including the last bank.
         * Bank count [11:8], bank-size exponent [19:16], last-bank exponent
         * [27:24]; each exponent describes bytes as 1 << (value + 2). */
        return (s->regs[off / 4] & 0xe7) | 0x0d0d0400;
    case 0x58:
        return 1; /* MULTICORE_HW_CFG: one encoding pipe. */
    case 0x64:
        return 64 - s->command_count; /* Command FIFO free words. */
    case 0x3d0:
        return 0x30300; /* CORE_REV */
    default:
        return s->regs[off / 4];
    }
}
static void topaz_write(void *opaque, hwaddr off, uint64_t value, unsigned size)
{
    IMGTopazState *s = opaque;
    if (off == 0x10) {
        s->regs[4 / 4] &= ~value;
        topaz_irq(s);
        return;
    }
    s->regs[off / 4] = value;
    if (off == 0xc)
        topaz_irq(s);
    if (off == 0 && (value & 1)) {
        /* GETVIDEO saves codec state outside volatile MTX hardware. Native
         * drivers reset the MTX during idle power gating, then SETVIDEO the
         * saved context before submitting the next picture. */
        if (!s->context_saved) {
            topaz_h264_free(s->encoder);
            s->encoder = NULL;
            s->frame_count = 0;
        }
        s->source_count = 0;
        s->reconstruction = 0;
        memset(s->coded, 0, sizeof(s->coded));
        s->firmware_loaded = s->firmware_started = false;
        s->command_count = 0;
        s->regs[0x104 / 4] = 0;
        s->regs[0x108 / 4] = 0;
        s->regs[4 / 4] = 0;
        topaz_irq(s);
    } else if (off == 0x404 && (value & 0x10000)) {
        bool ok = topaz_mtx_dma(s, value);
        if (ok && !(value & 0x4000000) && s->regs[0xb44 / 4] == TOPAZ_MTX_BASE)
            s->firmware_loaded = true;
        s->regs[off / 4] &= ~0x10000u;
        s->regs[0xb40 / 4] &= ~0x10000u;
        s->regs[0x40c / 4] = ok ? 0x20000 : 1;
    } else if (off == 0x8fc) {
        unsigned reg = value & 0xffff;
        if (value & 0x10000) {
            s->regs[0x8f8 / 4] = s->mtx_registers[reg];
        } else {
            s->mtx_registers[reg] = s->regs[0x8f8 / 4];
        }
        s->regs[off / 4] |= 0x80000000u;
    } else if (off == 0x880 && (value & 1) && s->firmware_loaded &&
               s->mtx_registers[5] == 0x80900000) {
        /* The firmware ABI's bootstrap handshake. Encoding commands use
         * the separate host FIFO and writeback buffers. */
        if (!s->firmware_started) {
            s->regs[0x104 / 4] = 0xed5;
            s->firmware_started = true;
        } else {
            topaz_commands(s);
        }
    } else if (off == 0x60 && s->command_count < G_N_ELEMENTS(s->commands)) {
        s->commands[s->command_count++] = value;
    } else if (off == 0x108 && s->firmware_started) {
        topaz_commands(s);
    }
}
static const MemoryRegionOps topaz_ops = {
    .read = topaz_read,
    .write = topaz_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = {.min_access_size = 4, .max_access_size = 4},
};
static void topaz_reset(DeviceState *dev)
{
    IMGTopazState *s = IMG_TOPAZ(dev);
    memset(s->regs, 0, sizeof(s->regs));
    memset(s->mtx_ram, 0, sizeof(s->mtx_ram));
    memset(s->mtx_registers, 0, sizeof(s->mtx_registers));
    memset(s->commands, 0, sizeof(s->commands));
    memset(s->coded, 0, sizeof(s->coded));
    memset(s->sources, 0, sizeof(s->sources));
    topaz_h264_free(s->encoder);
    s->encoder = NULL;
    s->source_count = s->frame_count = 0;
    s->reconstruction = 0;
    s->qp = 24;
    s->force_idr = false;
    s->context_saved = false;
    s->command_count = 0;
    s->firmware_loaded = false;
    s->firmware_started = false;
    s->video_context = s->width = s->height = 0;
    qemu_set_irq(s->irq, 0);
}
static void topaz_init(Object *obj)
{
    IMGTopazState *s = IMG_TOPAZ(obj);
    memory_region_init_io(&s->mmio, obj, &topaz_ops, s, TYPE_IMG_TOPAZ,
                          TOPAZ_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(s), &s->mmio);
    sysbus_init_irq(SYS_BUS_DEVICE(s), &s->irq);
}
static void topaz_finalize(Object *obj)
{
    topaz_h264_free(IMG_TOPAZ(obj)->encoder);
}
static int topaz_post_load(void *opaque, int version_id)
{
    IMGTopazState *s = opaque;
    /* Rebuild host-only codec state with a keyframe after migration. */
    topaz_h264_free(s->encoder);
    s->encoder = NULL;
    s->force_idr = true;
    topaz_irq(s);
    return 0;
}
static const VMStateDescription topaz_vmstate = {
    .name = TYPE_IMG_TOPAZ,
    .version_id = 2,
    .minimum_version_id = 1,
    .post_load = topaz_post_load,
    .fields = (const VMStateField[]){
        VMSTATE_UINT32_ARRAY(regs, IMGTopazState, TOPAZ_SIZE / 4),
        VMSTATE_UINT8_ARRAY(mtx_ram, IMGTopazState, TOPAZ_MTX_SIZE),
        VMSTATE_UINT32_ARRAY(mtx_registers, IMGTopazState, 0x10000),
        VMSTATE_UINT32_ARRAY(commands, IMGTopazState, 64),
        VMSTATE_UINT32(command_count, IMGTopazState),
        VMSTATE_UINT32_2DARRAY(coded, IMGTopazState, 4, 3),
        VMSTATE_UINT32_2DARRAY(sources, IMGTopazState, 8, 3),
        VMSTATE_UINT32(source_count, IMGTopazState),
        VMSTATE_UINT32(qp, IMGTopazState),
        VMSTATE_UINT32(frame_count, IMGTopazState),
        VMSTATE_UINT32_V(reconstruction, IMGTopazState, 2),
        VMSTATE_BOOL(force_idr, IMGTopazState),
        VMSTATE_BOOL(context_saved, IMGTopazState),
        VMSTATE_BOOL(firmware_loaded, IMGTopazState),
        VMSTATE_BOOL(firmware_started, IMGTopazState),
        VMSTATE_UINT32(video_context, IMGTopazState),
        VMSTATE_UINT32(width, IMGTopazState),
        VMSTATE_UINT32(height, IMGTopazState), VMSTATE_END_OF_LIST()}};
static void topaz_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, topaz_reset);
    dc->vmsd = &topaz_vmstate;
    dc->desc = "Imagination Topaz HP video encoder";
}
static const TypeInfo topaz_info = {
    .name = TYPE_IMG_TOPAZ,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IMGTopazState),
    .instance_init = topaz_init,
    .instance_finalize = topaz_finalize,
    .class_init = topaz_class_init,
};
static void topaz_register(void)
{
    type_register_static(&topaz_info);
}
type_init(topaz_register)
