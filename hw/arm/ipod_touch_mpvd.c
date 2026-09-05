#include "hw/arm/ipod_touch_mpvd.h"
#include "hw/arm/ipod_video.h"
#include "migration/vmstate.h"
#include "hw/core/cpu.h"
#include "hw/irq.h"
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
 * IT_MPVD_DECODE enables an experimental native MPEG-4 I/P decoder. It consumes
 * guest DMA data and signals completion; decoded planes are presented by the
 * opt-in LCD compositor (see docs/ipod-media.md). Without the opt-in, only register backing is active.
 */

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
    }
    size_t length = end - start;
    g_autofree uint8_t *data = g_malloc(length);
    if (address_space_read(&address_space_memory, start,
                           MEMTXATTRS_UNSPECIFIED, data, length)) {
        return false;
    }
    return ipod_video_frame(d->video, data, length, y, uv);
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
    if (getenv("IT_MPVD_DECODE") &&
        (addr == 0 || addr == 0x10000 || addr == 0x50000 ||
         addr == 0x30100 || addr == 0x60000)) {
        s->regs[addr / 4] &= ~(uint32_t)val;
        if (!s->regs[0]) {
            qemu_irq_lower(s->irq);
        }
    } else {
        s->regs[addr / 4] = (uint32_t)val;
    }
    if (getenv("IT_MPVD_DECODE") && addr == 0x1000c && val == 0x0c) {
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
    qemu_irq_lower(s->irq);
    memset(s->regs, 0, sizeof(s->regs));
}

static void ipod_touch_mpvd_finalize(Object *obj)
{
    mpvd_decoder_close(IPOD_TOUCH_MPVD(obj));
}

static const VMStateDescription vmstate_ipod_touch_mpvd = {
    .name = "ipod_touch_mpvd",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IPodTouchMPVDState, MPVD_REG_SIZE / 4),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_mpvd_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->vmsd = &vmstate_ipod_touch_mpvd;
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
