/*
 * VXD (Imagination MSVDX with an MTX core), the H.264/MPEG-4 decoder of the
 * S5L8920 and A4 (DT vxd,s5l8930x / vxd,s5l8920x; AppleVXD375).
 *
 * The MTX runs Imagination's DXVA firmware (BUILD_DXVA_FW1.00.10.0963 inside
 * AppleVXD375). This model doesn't run it: it plays its part (H) at the
 * firmware's interfaces, the comms area at +0x2fd8..+0x2fff and the two
 * message rings in MTX data RAM, which the host sees at +0x2000.
 *
 * Only H.264 decodes (s5l8920_vxd_h264.c); MPEG-4 renders complete without a
 * picture. IT_VXD_TRACE=FILE logs the conversation to FILE.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/address-spaces.h"
#include "qemu/log.h"
#include "qom/object.h"
#include "qemu/bswap.h"
#include "migration/vmstate.h"
#include "s5l8920_vxd_h264.h"

#define TYPE_S5L8920_VXD "s5l8920.vxd"
OBJECT_DECLARE_SIMPLE_TYPE(S5L8920VXDState, S5L8920_VXD)

/* Core registers (offsets as in Imagination's MSVDX register map). */
#define MTX_ENABLE          0x000
#define MTX_KICK            0x080
#define MTX_RW_DATA         0x0f8
#define MTX_RW_REQUEST      0x0fc
#define   MTX_DREADY        0x80000000u
#define MTX_RAM_DATA        0x104
#define MTX_RAM_CONTROL     0x108
#define MTX_RAM_STATUS      0x10c
#define MTX_SYSC_CDMAS0     0x348
#define DMAC_COUNT          0x504
#define   DMAC_EN           0x10000
#define DMAC_IRQ_STAT       0x50c
#define   DMAC_FIN          0x20000
#define VXD_CONTROL         0x600
#define   VXD_SOFT_RESETS   0x11110100u
#define VXD_INT_STATUS      0x608
#define VXD_INT_CLEAR       0x60c
#define VXD_HOST_INT_ENABLE 0x610
#define   VXD_INT_MTX       0x4000
#define MTX_RAM_BANK        0x6f0

/* The comms area, as AppleVXD375 2.53.1 lays it out (MSVDXStart, sendMessage). */
#define COMMS_BASE          0x2000
#define COMMS_FLAGS         0x2fd8
#define COMMS_SIGNATURE     0x2fe0
#define   COMMS_SIGNATURE_VALUE 0xa5a5a5a5u
#define COMMS_TO_HOST_BUF   0x2fe4  /* byte offset from 0x2000 << 16 | size in words */
#define COMMS_TO_HOST_RD    0x2fe8
#define COMMS_TO_HOST_WR    0x2fec
#define COMMS_TO_MTX_BUF    0x2ff0
#define COMMS_TO_MTX_RD     0x2ff4
#define COMMS_TO_MTX_WR     0x2ffc

/* Where this firmware puts its rings (its choice; the host reads them back). */
#define TO_MTX_OFF          0x800
#define TO_MTX_WORDS        0x100
#define TO_HOST_OFF         0xc00
#define TO_HOST_WORDS       0xf0

#define MSG_RENDER          0x81
#define MSG_CMD_COMPLETED   0xc0

#define MTX_RAM_WORDS       0x10000

struct S5L8920VXDState {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    qemu_irq irq;
    uint32_t regs[0x1000 / 4];
    uint32_t comms[0x1000 / 4];
    uint32_t ram[2][MTX_RAM_WORDS];   /* code (MCMID 0x10..), data (0x18..) */
    VXDH264 *h264;
    uint32_t ram_addr;                /* word address for MTX_RAM_DATA */
    FILE *trace;
};

#define VXD_LOG(s, ...) do { if ((s)->trace) fprintf((s)->trace, "vxd: " __VA_ARGS__); } while (0)

static void vxd_update_irq(S5L8920VXDState *s)
{
    qemu_set_irq(s->irq, !!(s->regs[VXD_INT_STATUS / 4] & s->regs[VXD_HOST_INT_ENABLE / 4]));
}

static uint32_t *vxd_comms(S5L8920VXDState *s, hwaddr off)
{
    return &s->comms[(off - COMMS_BASE) / 4];
}

/* The MSVDX MMU: a two-level table, 4 KiB pages, valid bit 0 (PD from the message, as reg 0x694). */
static bool vxd_translate(uint32_t ptd, uint32_t va, hwaddr *pa)
{
    uint32_t pde = address_space_ldl_le(&address_space_memory, (ptd & ~0xfffu) + (va >> 22) * 4,
                                        MEMTXATTRS_UNSPECIFIED, NULL);
    if (!(pde & 1)) {
        return false;
    }
    uint32_t pte = address_space_ldl_le(&address_space_memory, (pde & ~0xfffu) + ((va >> 12) & 0x3ff) * 4,
                                        MEMTXATTRS_UNSPECIFIED, NULL);
    if (!(pte & 1)) {
        return false;
    }
    *pa = (pte & ~0xfffu) | (va & 0xfff);
    return true;
}

/* Guest memory through the MMU, a page at a time. */
static bool vxd_rw(uint32_t ptd, uint32_t va, void *buf, size_t len, bool write)
{
    uint8_t *p = buf;

    while (len) {
        hwaddr pa;
        size_t n = MIN(len, 0x1000 - (va & 0xfff));
        if (!vxd_translate(ptd, va, &pa) ||
            address_space_rw(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, p, n, write) != MEMTX_OK) {
            return false;
        }
        p += n;
        va += n;
        len -= n;
    }
    return true;
}

/* What one render command buffer asks of the decoder (psb's DXVA command set). */
typedef struct VXDRender {
    uint32_t fe[5];          /* VEC H264 FE SPS0, PPS0, CUR_PIC0, SLICE0, SLICE1 */
    uint32_t sr_offset;
    uint32_t bs_va, bs_len;  /* the bitstream LLDMA, to the VEC shift register */
    uint32_t size, mode, luma, chroma;
} VXDRender;

#define CMD_REGVALPAIR_WRITE 0x1
#define CMD_RENDEC_WRITE     0x2
#define CMD_COMPLETION       0x6
#define CMD_HEADER           0x7
#define CMD_LLDMA            0xa
#define CMD_SR_SETUP         0xb
#define CMD_SLLDMA           0xc
#define VEC_H264_FE_SPS0     0x04800a00
#define LLDMA_PERIPH_SHIFTREG 0x820
#define CMDS_DISPLAY_PICTURE_SIZE 0x1000   /* MSVDX_CMDS registers, as RENDEC chunks address them */
#define CMDS_OPERATING_MODE       0x1008
#define CMDS_LUMA_RECONSTRUCTED   0x100c
#define CMDS_CHROMA_RECONSTRUCTED 0x1010

static bool vxd_parse_render(uint32_t ptd, uint32_t lldma, VXDRender *r)
{
    uint32_t d[8];

    if (!vxd_rw(ptd, lldma, d, sizeof(d), false) || d[2] != 0x350) {
        return false;
    }
    unsigned words = d[1] & 0xffff;
    g_autofree uint32_t *c = g_new(uint32_t, words + 8);
    if (!vxd_rw(ptd, d[6], c, words * 4, false)) {
        return false;
    }
    memset(r, 0, sizeof(*r));
    for (unsigned i = 0; i < words;) {
        unsigned n = c[i] & 0xfffff;
        switch (c[i] >> 28) {
        case CMD_HEADER:
            i += 5;
            break;
        case CMD_REGVALPAIR_WRITE:
            for (unsigned k = 0; k < n && i + 2 + 2 * k < words; k++) {
                uint32_t reg = c[i + 1 + 2 * k];
                if (reg >= VEC_H264_FE_SPS0 && reg < VEC_H264_FE_SPS0 + 0x14) {
                    r->fe[(reg - VEC_H264_FE_SPS0) / 4] = c[i + 2 + 2 * k];
                }
            }
            i += 1 + 2 * n;
            break;
        case CMD_RENDEC_WRITE:
            /*
             * Chunks: a header ((2 * words - 1) << 16 | register << 2 | 3), then the words for that register and
             * the ones after it. Wanted: MSVDX_CMDS' picture size, operating mode and the reconstructed
             * picture's two bases.
             */
            for (unsigned k = i + 1; k < i + 1 + n && k < words;) {
                unsigned count = ((c[k] >> 16) + 1) / 2, reg = (c[k] & 0xffff) >> 2;
                for (unsigned j = 0; j < count && k + 1 + j < words; j++) {
                    uint32_t v = c[k + 1 + j];
                    switch (reg + 4 * j) {
                    case CMDS_DISPLAY_PICTURE_SIZE: r->size = v; break;
                    case CMDS_OPERATING_MODE: r->mode = v; break;
                    case CMDS_LUMA_RECONSTRUCTED: r->luma = v; break;
                    case CMDS_CHROMA_RECONSTRUCTED: r->chroma = v; break;
                    }
                }
                k += 1 + count;
            }
            i += 1 + n;
            break;
        case CMD_SR_SETUP:
            r->sr_offset = c[i + 1];
            i += 3;
            break;
        case CMD_LLDMA:
        case CMD_SLLDMA:
            if (vxd_rw(ptd, (c[i] & 0x0fffffff) << 4, d, sizeof(d), false) && d[2] == LLDMA_PERIPH_SHIFTREG) {
                r->bs_va = d[6];
                r->bs_len = d[1] & 0xffff;
            }
            i++;
            break;
        case CMD_COMPLETION:
            i = words;
            break;
        default:
            i++;
            break;
        }
    }
    return r->bs_va && r->luma;
}

/*
 * Decode one slice: the bitstream DMA ends where its NAL ends, and the guest's
 * buffer holds the sample as the movie stores it (a 4-byte length before each
 * NAL), so the NAL's start is found from its length.
 */
static int vxd_decode(S5L8920VXDState *s, uint32_t ptd, const VXDRender *r)
{
    enum { BACK = 64 };
    static const unsigned strides[8] = { 384, 768, 1280, 1920, 512, 1024, 2048, 4096 };
    uint32_t base = r->bs_va - BACK, end = r->bs_va + r->bs_len;
    g_autofree uint8_t *buf = g_malloc(BACK + r->bs_len);

    if (r->bs_va < BACK || !vxd_rw(ptd, base, buf, BACK + r->bs_len, false)) {
        return -1;
    }
    int nal = -1;
    for (int i = BACK - 4; i >= 0 && nal < 0; i--) {
        uint8_t h = buf[i + 4];
        if (ldl_be_p(buf + i) == end - (base + i + 4) && !(h & 0x80) && ((h & 0x1f) == 1 || (h & 0x1f) == 5)) {
            nal = i + 4;
        }
    }
    if (nal < 0) {
        VXD_LOG(s, "no NAL before %08x (len %x sr %x)\n", r->bs_va, r->bs_len, r->sr_offset);
        return -1;
    }
    /* The picture's last slice unless the sample's next NAL is a slice that doesn't start at macroblock 0. */
    uint8_t next[6];
    bool last = !vxd_rw(ptd, end, next, sizeof(next), false) || ldl_be_p(next) - 1 > 0x3fffff ||
                (next[4] & 0x80) || ((next[4] & 0x1f) != 1 && (next[4] & 0x1f) != 5) || (next[5] & 0x80);
    VXDSlice sl = {
        .nal = buf + nal, .len = BACK + r->bs_len - nal,
        .sr_bit = (BACK - nal) * 8 + r->sr_offset,
        .sps0 = r->fe[0], .pps0 = r->fe[1], .pic0 = r->fe[2], .slice0 = r->fe[3], .slice1 = r->fe[4],
        .last = last,
    };
    VXDPicture pic;
    int got = vxd_h264_decode(s->h264, &sl, &pic);
    if (got <= 0) {
        return got;
    }
    unsigned stride = strides[(r->mode >> 24) & 7];
    unsigned w = MIN((unsigned)pic.width, stride), hgt = pic.height;
    g_autofree uint8_t *row = g_malloc(w);
    for (unsigned y = 0; y < hgt; y++) {
        vxd_rw(ptd, r->luma + y * stride, (void *)(pic.plane[0] + y * pic.stride[0]), w, true);
    }
    for (unsigned y = 0; y < hgt / 2; y++) {
        for (unsigned x = 0; x < w / 2; x++) {
            row[2 * x] = pic.plane[1][y * pic.stride[1] + x];
            row[2 * x + 1] = pic.plane[2][y * pic.stride[2] + x];
        }
        vxd_rw(ptd, r->chroma + y * stride, row, w, true);
    }
    VXD_LOG(s, "picture %ux%u to %08x/%08x stride %u\n", w, hgt, r->luma, r->chroma, stride);
    return 1;
}

/* The firmware's start: its signature and where its rings are. */
static void vxd_firmware_start(S5L8920VXDState *s)
{
    *vxd_comms(s, COMMS_TO_MTX_BUF) = TO_MTX_OFF << 16 | TO_MTX_WORDS;
    *vxd_comms(s, COMMS_TO_HOST_BUF) = TO_HOST_OFF << 16 | TO_HOST_WORDS;
    *vxd_comms(s, COMMS_SIGNATURE) = COMMS_SIGNATURE_VALUE;
    VXD_LOG(s, "firmware started (flags %x)\n", *vxd_comms(s, COMMS_FLAGS));
}

static void vxd_post(S5L8920VXDState *s, const uint32_t *msg, unsigned words)
{
    uint32_t wr = *vxd_comms(s, COMMS_TO_HOST_WR);
    for (unsigned i = 0; i < words; i++) {
        s->comms[TO_HOST_OFF / 4 + wr] = msg[i];
        wr = (wr + 1) % TO_HOST_WORDS;
    }
    *vxd_comms(s, COMMS_TO_HOST_WR) = wr;
    s->regs[VXD_INT_STATUS / 4] |= VXD_INT_MTX;
    vxd_update_irq(s);
}

static void vxd_message(S5L8920VXDState *s, const uint32_t *msg, unsigned words)
{
    uint8_t id = msg[0] >> 8;

    VXDRender r;
    int got = id == MSG_RENDER && words >= 3 && vxd_parse_render(msg[1], msg[2], &r) ? vxd_decode(s, msg[1], &r) : -2;
    VXD_LOG(s, "message %02x fence %u slice %08x: %s\n", id, words > 4 ? msg[4] : 0, words > 6 ? msg[6] : 0,
            got == 1 ? "picture" : got == 0 ? "slice" : got == -1 ? "FAILED" : "not a render");
    /* ponytail: every message completes at once, decoded or not; fence = word 4 (psb's FW_VA_RENDER). */
    uint32_t done[3] = { MSG_CMD_COMPLETED << 8 | 12, words > 4 ? msg[4] : 0, 0 };
    vxd_post(s, done, 3);
}

static void vxd_kick(S5L8920VXDState *s)
{
    uint32_t rd = *vxd_comms(s, COMMS_TO_MTX_RD), wr = *vxd_comms(s, COMMS_TO_MTX_WR);

    while (rd != wr && rd < TO_MTX_WORDS) {
        uint32_t msg[TO_MTX_WORDS];
        unsigned words = ((s->comms[TO_MTX_OFF / 4 + rd] & 0xff) + 3) / 4;
        if (!words) {
            rd = 0;   /* a padding word: the host wrapped */
            continue;
        }
        for (unsigned i = 0; i < words && i < TO_MTX_WORDS; i++) {
            msg[i] = s->comms[TO_MTX_OFF / 4 + rd];
            rd = (rd + 1) % TO_MTX_WORDS;
        }
        vxd_message(s, msg, MIN(words, TO_MTX_WORDS));
    }
    *vxd_comms(s, COMMS_TO_MTX_RD) = rd;
}

static uint64_t vxd_read(void *opaque, hwaddr off, unsigned size)
{
    S5L8920VXDState *s = opaque;
    uint32_t v;

    if (off >= COMMS_BASE && off < COMMS_BASE + 0x1000) {
        return *vxd_comms(s, off);
    }
    if (off >= 0x1000) {
        VXD_LOG(s, "read %05" HWADDR_PRIx "\n", off);
        return 0;
    }
    switch (off) {
    case MTX_RW_REQUEST:
        v = s->regs[off / 4] | MTX_DREADY;
        break;
    case MTX_RAM_STATUS:
    case MTX_SYSC_CDMAS0:
        v = 1;
        break;
    case MTX_RAM_DATA: {
        uint32_t ctl = s->regs[MTX_RAM_CONTROL / 4];
        v = s->ram[((ctl >> 20) & 0xff) >= 0x18][s->ram_addr % MTX_RAM_WORDS];
        if (ctl & 2) {
            s->ram_addr++;
        }
        break;
    }
    case MTX_RAM_BANK:
        v = 0xd << 16;   /* 32 KiB banks */
        break;
    default:
        v = s->regs[off / 4];
        break;
    }
    return v;
}

static void vxd_write(void *opaque, hwaddr off, uint64_t val, unsigned size)
{
    S5L8920VXDState *s = opaque;

    if (off >= COMMS_BASE && off < COMMS_BASE + 0x1000) {
        *vxd_comms(s, off) = val;
        return;
    }
    if (off >= 0x1000) {
        VXD_LOG(s, "write %05" HWADDR_PRIx " = %08" PRIx64 "\n", off, val);
        return;
    }
    switch (off) {
    case MTX_ENABLE:
        s->regs[off / 4] = val;
        if (val & 1) {
            vxd_firmware_start(s);
        }
        return;
    case MTX_KICK:
        vxd_kick(s);
        return;
    case MTX_RAM_CONTROL:
        s->regs[off / 4] = val;
        s->ram_addr = (val >> 2) & 0x3ffff;
        return;
    case MTX_RAM_DATA: {
        uint32_t ctl = s->regs[MTX_RAM_CONTROL / 4];
        s->ram[((ctl >> 20) & 0xff) >= 0x18][s->ram_addr % MTX_RAM_WORDS] = val;
        if (ctl & 2) {
            s->ram_addr++;
        }
        return;
    }
    case DMAC_COUNT:
        s->regs[off / 4] = val;
        if (val & DMAC_EN) {
            /* ponytail: the firmware download isn't copied into MTX RAM; nothing here runs it. */
            VXD_LOG(s, "DMAC from %08x, %u words to periph %x\n", s->regs[0x500 / 4],
                    (unsigned)(val & 0xffff), s->regs[0x514 / 4]);
            s->regs[DMAC_IRQ_STAT / 4] |= DMAC_FIN;
        }
        return;
    case VXD_CONTROL:
        s->regs[off / 4] = val & ~VXD_SOFT_RESETS;   /* resets finish at once */
        return;
    case VXD_INT_CLEAR:
        s->regs[VXD_INT_STATUS / 4] &= ~val;
        vxd_update_irq(s);
        return;
    case VXD_HOST_INT_ENABLE:
        s->regs[off / 4] = val;
        vxd_update_irq(s);
        return;
    default:
        VXD_LOG(s, "reg %03" HWADDR_PRIx " = %08" PRIx64 "\n", off, val);
        s->regs[off / 4] = val;
        return;
    }
}

static const MemoryRegionOps vxd_ops = {
    .read = vxd_read,
    .write = vxd_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4 },
};

static void vxd_init(Object *obj)
{
    S5L8920VXDState *s = S5L8920_VXD(obj);

    s->h264 = vxd_h264_new();
    memory_region_init_io(&s->iomem, obj, &vxd_ops, s, TYPE_S5L8920_VXD, 0x100000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
    const char *trace = getenv("IT_VXD_TRACE");
    if (trace && *trace) {
        s->trace = fopen(trace, "a");
        if (s->trace) {
            setvbuf(s->trace, NULL, _IOLBF, 0);
        }
    }
}

static void vxd_reset(DeviceState *dev)
{
    S5L8920VXDState *s = S5L8920_VXD(dev);

    memset(s->regs, 0, sizeof(s->regs));
    memset(s->comms, 0, sizeof(s->comms));
    s->ram_addr = 0;
}

/*
 * The registers and the comms area (the rings, their indexes, the firmware's signature) are the state: the host
 * decoder starts over, so a restored device decodes again from the stream's next IDR. MTX RAM is left out: only
 * a firmware upload's read-back looks at it.
 */
static const VMStateDescription vxd_vmstate = {
    .name = TYPE_S5L8920_VXD,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, S5L8920VXDState, 0x1000 / 4),
        VMSTATE_UINT32_ARRAY(comms, S5L8920VXDState, 0x1000 / 4),
        VMSTATE_UINT32(ram_addr, S5L8920VXDState),
        VMSTATE_END_OF_LIST()
    },
};

static void vxd_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    device_class_set_legacy_reset(dc, vxd_reset);
    dc->vmsd = &vxd_vmstate;
    dc->desc = "S5L8920/A4 VXD (MSVDX) H.264 decoder";
}

static const TypeInfo vxd_info = {
    .name = TYPE_S5L8920_VXD,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8920VXDState),
    .instance_init = vxd_init,
    .class_init = vxd_class_init,
};

static void vxd_register_types(void)
{
    type_register_static(&vxd_info);
}

type_init(vxd_register_types)
