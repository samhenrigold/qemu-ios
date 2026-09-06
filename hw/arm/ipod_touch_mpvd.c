#include "hw/arm/ipod_touch_mpvd.h"
#include "hw/arm/ipod_video.h"
#include "migration/vmstate.h"
#include "migration/qemu-file.h"
#include "hw/core/cpu.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "target/arm/cpu.h"
#include "trace.h"
#include "exec/address-spaces.h"
#include "qemu/error-report.h"
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <CoreMedia/CoreMedia.h>
#include <CoreVideo/CoreVideo.h>
#include <VideoToolbox/VideoToolbox.h>
#endif

/*
 * MPVD (video decode) register window.
 *
 * The device tree has mpvd@1600000 with seven 0x1000 reg windows spanning
 * 0x39600000..0x39660fff, but nothing was mapped there, so AppleMPVDDriver's power-state
 * path took a synchronous external abort the first time it touched the block --
 * the "Memory access exception" panic with fsr=0x808, far=0xec3fd01c (the
 * driver's iomap of this window, offset 0x1c). That happens right after
 * "enabling idle sleep", so every boot died once the device went idle.
 *
 * The mpvd-decode machine option enables an experimental native MPEG-4 I/P decoder. It consumes
 * guest DMA data and signals completion; decoded planes are presented by the
 * opt-in LCD compositor (see docs/ipod-media.md). Without the opt-in, only register backing is active.
 */

/* Bound retained compressed inputs, not playback duration. Overflow prevents
 * saving until the next I-VOP or reset; the live decoder continues unchanged. */
#define MPVD_HISTORY_PACKETS 4096
#define MPVD_HISTORY_BYTES (16 * 1024 * 1024)
#define MPVD_PACKET_BYTES (4 * 1024 * 1024 + 128)

static void mpvd_history_clear(IPodTouchMPVDState *s)
{
    g_clear_pointer(&s->packets, g_ptr_array_unref);
    s->packet_bytes = 0;
    s->replay_width = s->replay_height = s->replay_time_bits = 0;
    s->history_unavailable = false;
}

static void mpvd_history_lost(IPodTouchMPVDState *s)
{
    mpvd_history_clear(s);
    s->history_unavailable = true;
}

static int mpvd_packet_type(const uint8_t *data, size_t length)
{
    return length >= 5 && length <= MPVD_PACKET_BYTES &&
           !memcmp(data, "\0\0\1\xb6", 4) ? data[4] >> 6 : -1;
}

static void mpvd_history_append(IPodTouchMPVDState *s, const uint8_t *data,
                                size_t length, unsigned width, unsigned height,
                                unsigned time_bits)
{
    int type = mpvd_packet_type(data, length);
    if (type == 0) {
        mpvd_history_clear(s);
        s->packets = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
        s->replay_width = width;
        s->replay_height = height;
        s->replay_time_bits = time_bits;
    }
    if (s->history_unavailable || !s->packets || type < 0 || type > 1 ||
        s->packets->len >= MPVD_HISTORY_PACKETS ||
        length > MPVD_HISTORY_BYTES - s->packet_bytes) {
        mpvd_history_lost(s);
        return;
    }
    g_ptr_array_add(s->packets, g_bytes_new(data, length));
    s->packet_bytes += length;
}

#ifdef __APPLE__
typedef struct MPVDDecoder {
    IPodVideoDecoder *video;
    unsigned width, height, time_bits;
} MPVDDecoder;

static void mpvd_bits(uint8_t *buf, unsigned *pos, uint32_t value, unsigned count)
{
    for (unsigned i = count; i; i--, (*pos)++) {
        buf[*pos / 8] |= ((value >> (i - 1)) & 1) << (7 - *pos % 8);
    }
}

static CMVideoFormatDescriptionRef mpvd_make_format(unsigned width,
                                                    unsigned height,
                                                    unsigned time_bits)
{
    if (!width || !height || width > 2048 || height > 2048 ||
        !time_bits || time_bits > 16) {
        return NULL;
    }
    /* Decode to raw full-range SD planes. The guest owns presentation/color
     * conversion; allowing the host to choose a range changes coded samples.
     * The engine supplies the time-increment bit width, not the clock rate.
     * I/P reconstruction needs that width; B-picture timing is not supported. */
    uint8_t vol[64] = { 0, 0, 1, 0xb0, 0xf5, 0, 0, 1, 0xb5,
        0x0e, 0xe0, 0x40, 0xc0, 0xcf, 0, 0, 1, 0, 0, 0, 1, 0x20 };
    unsigned pos = 22 * 8;
    mpvd_bits(vol, &pos, 0, 1); /* random_accessible_vol */
    mpvd_bits(vol, &pos, 1, 8); /* video_object_type_indication */
    mpvd_bits(vol, &pos, 0, 1); /* version 1 */
    mpvd_bits(vol, &pos, 1, 4); /* square pixels */
    mpvd_bits(vol, &pos, 0, 1); /* no vol_control_parameters */
    mpvd_bits(vol, &pos, 0, 2); /* rectangular shape */
    mpvd_bits(vol, &pos, 1, 1);
    mpvd_bits(vol, &pos, (1u << time_bits) - 1, 16);
    mpvd_bits(vol, &pos, 1, 1);
    mpvd_bits(vol, &pos, 0, 1); /* variable vop rate */
    mpvd_bits(vol, &pos, 1, 1);
    mpvd_bits(vol, &pos, width, 13);
    mpvd_bits(vol, &pos, 1, 1);
    mpvd_bits(vol, &pos, height, 13);
    mpvd_bits(vol, &pos, 1, 1);
    mpvd_bits(vol, &pos, 0, 1); /* progressive */
    mpvd_bits(vol, &pos, 1, 1); /* obmc_disable */
    mpvd_bits(vol, &pos, 0, 1); /* no sprite */
    mpvd_bits(vol, &pos, 0, 1); /* 8-bit samples */
    mpvd_bits(vol, &pos, 0, 1); /* scalar quantization */
    mpvd_bits(vol, &pos, 1, 1); /* complexity_estimation_disable */
    mpvd_bits(vol, &pos, 0, 1); /* resync markers permitted */
    mpvd_bits(vol, &pos, 0, 1); /* no data partition */
    mpvd_bits(vol, &pos, 0, 1); /* no scalability */
    mpvd_bits(vol, &pos, 0, 1); /* byte-align with MPEG-4 stuffing */
    while (pos % 8) {
        mpvd_bits(vol, &pos, 1, 1);
    }
    unsigned n = pos / 8;
    /* ES_Descriptor, DecoderConfigDescriptor, DecoderSpecificInfo, SLConfig. */
    uint8_t esds[128] = { 0, 0, 0, 0, 3, n + 23, 0, 0, 0,
        4, n + 15, 0x20, 0x11, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 5, n };
    memcpy(esds + 26, vol, n);
    esds[26 + n] = 6;
    esds[27 + n] = 1;
    esds[28 + n] = 2;
    CFDataRef data = CFDataCreate(NULL, esds, n + 29);
    if (!data) {
        return NULL;
    }
    const void *keys[] = { CFSTR("esds") }, *values[] = { data };
    CFDictionaryRef atoms = CFDictionaryCreate(NULL, keys, values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(data);
    if (!atoms) {
        return NULL;
    }
    const void *ext_keys[] = {
        kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms };
    const void *ext_values[] = { atoms };
    CFDictionaryRef extensions = CFDictionaryCreate(NULL, ext_keys, ext_values, 1,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFRelease(atoms);
    if (!extensions) {
        return NULL;
    }
    CMVideoFormatDescriptionRef format = NULL;
    CMVideoFormatDescriptionCreate(NULL, kCMVideoCodecType_MPEG4Video,
                                   width, height, extensions, &format);
    CFRelease(extensions);
    return format;
}

static void mpvd_decoder_close(IPodTouchMPVDState *s)
{
    MPVDDecoder *d = s->decoder;
    if (d) {
        ipod_video_close(d->video);
        g_free(d);
        s->decoder = NULL;
    }
}

/* Only DMA to the board's DRAM, never to another peripheral. */
static bool mpvd_ram(uint32_t addr, size_t size)
{
    return addr >= 0x08000000 && addr < 0x10000000 &&
           size <= 0x10000000 - addr;
}

static bool mpvd_decode(IPodTouchMPVDState *s)
{
    unsigned width = (s->regs[0x6006c / 4] >> 16) * 16;
    unsigned height = (s->regs[0x6006c / 4] & 0xffff) * 16;
    unsigned time_bits = s->regs[0x1009c / 4] & 15;
    uint32_t ptr = s->regs[0x60018 / 4], end = s->regs[0x6001c / 4];
    uint32_t y = s->regs[0x6003c / 4], uv = s->regs[0x60044 / 4];
    uint32_t ctrl = s->regs[0x10010 / 4];
    uint8_t prefix[132];
    uint32_t start = 0;
    MPVDDecoder *d = s->decoder;

    /* Initial support: rectangular 8-bit MPEG-4 I/P jobs. B frames, quarter
     * samples and alternate scan need their full hardware configuration. */
    if (!width || !height || width > 2048 || height > 2048 || !time_bits ||
        (ctrl & 3) > 1 || (ctrl & ((1u << 17) | (1u << 11) | (1u << 5))) ||
        (s->regs[0x41804 / 4] & 0x180) ||
        ptr < 0x08000080 || end <= ptr || end - ptr > 4 * 1024 * 1024 ||
        !mpvd_ram(ptr - 128, sizeof(prefix)) ||
        !mpvd_ram(y, (size_t)width * height) ||
        !mpvd_ram(uv, (size_t)width * height / 2)) {
        return false;
    }
    if (address_space_read(&address_space_memory, ptr - 128,
                           MEMTXATTRS_UNSPECIFIED, prefix, sizeof(prefix))) {
        return false;
    }
    /* The MI pointer is word-aligned after the already-parsed VOP header.
     * Recover that packet's start code, with a bounded header look-behind. */
    for (int i = 128; i >= 0; i--) {
        if (!memcmp(prefix + i, "\0\0\1\xb6", 4)) {
            start = ptr - 128 + i;
            break;
        }
    }
    if (!start || !mpvd_ram(start, end - start)) {
        return false;
    }
    size_t length = end - start;
    g_autofree uint8_t *data = g_malloc(length);
    if (address_space_read(&address_space_memory, start,
                           MEMTXATTRS_UNSPECIFIED, data, length) ||
        mpvd_packet_type(data, length) != (ctrl & 3)) {
        return false;
    }
    if (s->packets && (s->replay_width != width ||
        s->replay_height != height || s->replay_time_bits != time_bits)) {
        mpvd_history_clear(s);
    }
    if (!d || d->width != width || d->height != height ||
        d->time_bits != time_bits) {
        mpvd_decoder_close(s);
        d = g_new0(MPVDDecoder, 1);
        s->decoder = d;
        d->width = width;
        d->height = height;
        d->time_bits = time_bits;
        CMVideoFormatDescriptionRef format = mpvd_make_format(width, height, time_bits);
        d->video = ipod_video_create(format, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange);
        if (format) {
            CFRelease(format);
        }
        if (!d->video) {
            mpvd_decoder_close(s);
            return false;
        }
        /* Rebuild native references only; replay must never touch guest DMA. */
        for (unsigned i = 0; s->packets && i < s->packets->len; i++) {
            size_t replay_length;
            const uint8_t *replay = g_bytes_get_data(s->packets->pdata[i], &replay_length);
            if (!ipod_video_frame(d->video, (uint8_t *)replay, replay_length, 0, 0)) {
                mpvd_decoder_close(s);
                mpvd_history_lost(s);
                return false;
            }
        }
    }
    bool ok = ipod_video_frame(d->video, data, length, y, uv);
    if (ok) {
        mpvd_history_append(s, data, length, width, height, time_bits);
    } else {
        /* Native references may advance even when output DMA fails. */
        mpvd_history_lost(s);
    }
    return ok;
}

#else
static void mpvd_decoder_close(IPodTouchMPVDState *s) {}
static bool mpvd_decode(IPodTouchMPVDState *s) { return false; }
#endif

static uint64_t ipod_touch_mpvd_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchMPVDState *s = (IPodTouchMPVDState *)opaque;

    if (addr + 4 > MPVD_REG_SIZE) {
        return 0;
    }
    uint32_t value = s->regs[addr / 4];
    trace_ipod_touch_mpvd_read(addr, value);
    return value;
}

static void ipod_touch_mpvd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchMPVDState *s = (IPodTouchMPVDState *)opaque;

    if (addr + 4 > MPVD_REG_SIZE) {
        return;
    }
    if (s->decode_enabled &&
        (addr == 0 || addr == 0x10000 || addr == 0x50000 ||
         addr == 0x30100 || addr == 0x60000)) {
        s->regs[addr / 4] &= ~(uint32_t)val;
        if (!s->regs[0]) {
            qemu_irq_lower(s->irq);
        }
    } else {
        s->regs[addr / 4] = (uint32_t)val;
    }
    if (s->decode_enabled && addr == 0x1000c && val == 0x0c) {
        bool ok = mpvd_decode(s);
        s->regs[0] = 2;
        s->regs[0x10000 / 4] = ok ? 4 : 1;
        s->regs[0x50000 / 4] = ok ? 2 : 0;
        s->regs[0x30100 / 4] = 0x20;
        qemu_set_irq(s->irq, 1);
    }
    if (trace_event_get_state_backends(TRACE_IPOD_TOUCH_MPVD_WRITE)) {
        CPUARMState *env = current_cpu ? &ARM_CPU(current_cpu)->env : NULL;
        trace_ipod_touch_mpvd_write(addr, val, env ? env->regs[15] : 0,
                                   env ? env->regs[14] : 0);
    }
}

static const MemoryRegionOps ipod_touch_mpvd_ops = {
    .read = ipod_touch_mpvd_read,
    .write = ipod_touch_mpvd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_mpvd_init(Object *obj)
{
    IPodTouchMPVDState *s = IPOD_TOUCH_MPVD(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_mpvd_ops, s,
                          TYPE_IPOD_TOUCH_MPVD, MPVD_REG_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

static void ipod_touch_mpvd_reset(DeviceState *dev)
{
    IPodTouchMPVDState *s = IPOD_TOUCH_MPVD(dev);

    mpvd_decoder_close(s);
    mpvd_history_clear(s);
    qemu_irq_lower(s->irq);
    memset(s->regs, 0, sizeof(s->regs));
}

static void ipod_touch_mpvd_finalize(Object *obj)
{
    mpvd_decoder_close(IPOD_TOUCH_MPVD(obj));
    mpvd_history_clear(IPOD_TOUCH_MPVD(obj));
}

static int mpvd_put_packets(QEMUFile *f, void *pv, size_t size,
                            const VMStateField *field, JSONWriter *vmdesc)
{
    GPtrArray *packets = *(GPtrArray **)pv;
    unsigned count = packets ? packets->len : 0;
    if (count > MPVD_HISTORY_PACKETS) return -E2BIG;
    qemu_put_be32(f, count);
    size_t total = 0;
    for (unsigned i = 0; i < count; i++) {
        size_t length;
        const uint8_t *data = g_bytes_get_data(packets->pdata[i], &length);
        if (mpvd_packet_type(data, length) != (i ? 1 : 0) ||
            length > MPVD_HISTORY_BYTES - total) return -EINVAL;
        total += length;
        qemu_put_be32(f, length);
        qemu_put_buffer(f, data, length);
    }
    return qemu_file_get_error(f);
}

static int mpvd_get_packets(QEMUFile *f, void *pv, size_t size,
                            const VMStateField *field)
{
    unsigned count = qemu_get_be32(f);
    if (count > MPVD_HISTORY_PACKETS) return -EINVAL;
    g_autoptr(GPtrArray) packets = g_ptr_array_new_with_free_func((GDestroyNotify)g_bytes_unref);
    size_t total = 0;
    for (unsigned i = 0; i < count; i++) {
        unsigned length = qemu_get_be32(f);
        if (length < 5 || length > MPVD_PACKET_BYTES ||
            length > MPVD_HISTORY_BYTES - total) return -EINVAL;
        total += length;
        g_autofree uint8_t *data = g_malloc(length);
        if (qemu_get_buffer(f, data, length) != length) return -EIO;
        if (mpvd_packet_type(data, length) != (i ? 1 : 0)) return -EINVAL;
        g_ptr_array_add(packets, g_bytes_new_take(g_steal_pointer(&data), length));
    }
    if (qemu_file_get_error(f)) return qemu_file_get_error(f);
    g_clear_pointer((GPtrArray **)pv, g_ptr_array_unref);
    *(GPtrArray **)pv = g_steal_pointer(&packets);
    return 0;
}

static const VMStateInfo vmstate_mpvd_packets = {
    .name = "mpvd-packets", .get = mpvd_get_packets, .put = mpvd_put_packets,
};

static int mpvd_pre_load(void *opaque)
{
    IPodTouchMPVDState *s = opaque;
    mpvd_decoder_close(s);
    mpvd_history_clear(s);
    return 0;
}

static int mpvd_pre_save(void *opaque)
{
    IPodTouchMPVDState *s = opaque;
    s->saved_decode_enabled = s->decode_enabled;
    if (s->decode_enabled && s->history_unavailable) {
        error_report("MPVD reference history unavailable (limit: 4096 pictures/16 MiB); "
                     "wait for an I-picture or reset before saving a snapshot");
        return -ENOTSUP;
    }
    return 0;
}

static int mpvd_post_load(void *opaque, int version_id)
{
    IPodTouchMPVDState *s = opaque;
    if (version_id >= 2 && s->saved_decode_enabled != s->decode_enabled) return -EINVAL;
    mpvd_decoder_close(s);
    unsigned count = s->packets ? s->packets->len : 0;
    if (version_id >= 3) {
        if (count) {
            if (!s->decode_enabled || !s->replay_width || !s->replay_height ||
                s->replay_width > 2048 || s->replay_height > 2048 ||
                (s->replay_width % 16) || (s->replay_height % 16) ||
                !s->replay_time_bits || s->replay_time_bits > 15) return -EINVAL;
        } else if (s->replay_width || s->replay_height || s->replay_time_bits) {
            return -EINVAL;
        }
    } else if (s->decode_enabled && s->regs[0x1000c / 4] == 0x0c) {
        /* Old snapshots contain no reference pictures. They can resume at an
         * I-picture, but must not produce a seemingly complete new snapshot. */
        s->history_unavailable = true;
    }
    s->packet_bytes = 0;
    for (unsigned i = 0; i < count; i++) {
        s->packet_bytes += g_bytes_get_size(s->packets->pdata[i]);
    }
    qemu_set_irq(s->irq, s->decode_enabled && s->regs[0] != 0);
    return 0;
}

static const VMStateDescription vmstate_ipod_touch_mpvd = {
    .name = "ipod_touch_mpvd",
    .version_id = 3,
    .minimum_version_id = 1,
    .pre_load = mpvd_pre_load,
    .pre_save = mpvd_pre_save,
    .post_load = mpvd_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IPodTouchMPVDState, MPVD_REG_SIZE / 4),
        VMSTATE_BOOL_V(saved_decode_enabled, IPodTouchMPVDState, 2),
        VMSTATE_UINT32_V(replay_width, IPodTouchMPVDState, 3),
        VMSTATE_UINT32_V(replay_height, IPodTouchMPVDState, 3),
        VMSTATE_UINT32_V(replay_time_bits, IPodTouchMPVDState, 3),
        VMSTATE_SINGLE(packets, IPodTouchMPVDState, 3, vmstate_mpvd_packets, GPtrArray *),
        VMSTATE_END_OF_LIST()
    }
};

static const Property mpvd_properties[] = {
    DEFINE_PROP_BOOL("decode", IPodTouchMPVDState, decode_enabled, false),
};

static void ipod_touch_mpvd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_mpvd;
    device_class_set_props(dc, mpvd_properties);
    device_class_set_legacy_reset(dc, ipod_touch_mpvd_reset);
}

static const TypeInfo ipod_touch_mpvd_type_info = {
    .name = TYPE_IPOD_TOUCH_MPVD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchMPVDState),
    .instance_init = ipod_touch_mpvd_init,
    .instance_finalize = ipod_touch_mpvd_finalize,
    .class_init = ipod_touch_mpvd_class_init,
};

static void ipod_touch_mpvd_register_types(void)
{
    type_register_static(&ipod_touch_mpvd_type_info);
}

type_init(ipod_touch_mpvd_register_types)
