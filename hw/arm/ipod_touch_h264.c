#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "hw/irq.h"
#include "hw/arm/ipod_video.h"
#include "exec/address-spaces.h"
#include "migration/vmstate.h"
#include "migration/qemu-file-types.h"
#include "qemu/error-report.h"
#include "qemu/bswap.h"
#include "qemu/host-utils.h"
#include "trace.h"
#ifdef IT_HAVE_AVCODEC
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>

#endif

/* Experimental 7E18 M2H264 bit reader and native progressive I/P
 * reconstruction. IT_H264_DECODE must be explicitly enabled by the machine. */
typedef struct IPodH264State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    uint32_t regs[0x4000 / 4];
    GByteArray *rbsp;
    unsigned bit;
    bool exhausted;
    qemu_irq irq;
    bool irq_level;
#ifdef IT_HAVE_AVCODEC
    AVCodecContext *codec;
    AVFrame *frame;
    bool partial;
    unsigned partial_start;
    uint32_t partial_key[3];
    uint32_t partial_references[16][2];
    unsigned partial_reference_count;
    GPtrArray *partial_slices;
    size_t partial_bytes;
#endif
#ifdef __APPLE__
    IPodVideoDecoder *video;
    CMVideoFormatDescriptionRef format;
#endif
} IPodH264State;

#ifdef IT_HAVE_AVCODEC
typedef struct H264SoftwareSlice {
    uint32_t regs[0xd8 / 4], weights[32];
    unsigned references[16], bit;
    GByteArray *rbsp;
} H264SoftwareSlice;

static void h264_software_slice_free(gpointer opaque)
{
    H264SoftwareSlice *slice = opaque;
    g_byte_array_unref(slice->rbsp);
    g_free(slice);
}
#endif

static void h264_set_irq(IPodH264State *s, bool level)
{
    s->irq_level = level;
    if (level) qemu_irq_raise(s->irq);
    else qemu_irq_lower(s->irq);
}

static void h264_decoder_close(IPodH264State *s)
{
#ifdef IT_HAVE_AVCODEC
    avcodec_free_context(&s->codec);
    av_frame_free(&s->frame);
    s->partial = false;
    g_clear_pointer(&s->partial_slices, g_ptr_array_unref);
    s->partial_bytes = 0;
#endif
#ifdef __APPLE__
    ipod_video_close(s->video);
    s->video = NULL;
    if (s->format) CFRelease(s->format);
    s->format = NULL;
#endif
}

static uint32_t h264_bits(IPodH264State *s, unsigned count)
{
    uint32_t value = 0;
    if (count > 32 || s->bit > s->rbsp->len * 8 ||
        count > s->rbsp->len * 8 - s->bit) {
        if (!s->exhausted) {
            warn_report("H264: bitstream read beyond input");
        }
        s->exhausted = true;
        return 0;
    }
    for (unsigned i = 0; i < count; i++, s->bit++) {
        value = (value << 1) |
            ((s->rbsp->data[s->bit / 8] >> (7 - s->bit % 8)) & 1);
    }
    return value;
}

static void h264_nal(IPodH264State *s)
{
    uint64_t base = (uint64_t)(s->regs[0x1200 / 4] & 0x3fffff) << 10;
    uint32_t start = s->regs[0x180c / 4] & 0x7fffff;
    uint32_t end = s->regs[0x1810 / 4] & 0x7fffff;
    g_autofree uint8_t *input = NULL;
    s->bit = 0;
    s->exhausted = false;
    g_byte_array_set_size(s->rbsp, 0);
    s->regs[0x1628 / 4] = 0x100; /* ready, invalid NAL type on DMA failure */
    if (end <= start || end - start > 4 * 1024 * 1024 ||
        base + start < 0x08000000 || base + end > 0x10000000) {
        return;
    }
    input = g_malloc(end - start);
    if (address_space_read(&address_space_memory, base + start,
                          MEMTXATTRS_UNSPECIFIED, input, end - start)) {
        return;
    }
    /* The driver supplies one NAL, after the container's length prefix.
     * 0x1628 reports nal_ref_idc/type and consumes its one-byte header. */
    s->regs[0x1628 / 4] |= input[0] & 0x7f;
    unsigned zeros = 0;
    for (unsigned i = 1; i < end - start; i++) {
        if (zeros == 2 && input[i] == 3) {
            zeros = 0;
            continue;
        }
        g_byte_array_append(s->rbsp, input + i, 1);
        zeros = input[i] == 0 ? MIN(zeros + 1, 2) : 0;
    }
}

/* Build a coded NAL from parsed hardware fields. The macroblock payload is
 * preserved bit-for-bit; original software-only SPS/PPS IDs are irrelevant. */
static void h264_put(uint8_t *buf, unsigned *bit, uint32_t value, unsigned n)
{
    for (unsigned i = n; i; i--, (*bit)++) {
        buf[*bit / 8] |= ((value >> (i - 1)) & 1) << (7 - *bit % 8);
    }
}

static void h264_ue(uint8_t *buf, unsigned *bit, unsigned value)
{
    unsigned n = 32 - clz32(value + 1);
    h264_put(buf, bit, 0, n - 1);
    h264_put(buf, bit, value + 1, n);
}

static void h264_se(uint8_t *buf, unsigned *bit, int value)
{
    h264_ue(buf, bit, value <= 0 ? -2 * value : 2 * value - 1);
}

static GByteArray *h264_escape(uint8_t *buf, unsigned bit)
{
    GByteArray *nal = g_byte_array_new();
    unsigned zeros = 0;
    for (unsigned i = 0; i < (bit + 7) / 8; i++) {
        if (zeros == 2 && buf[i] <= 3) {
            uint8_t escape = 3;
            g_byte_array_append(nal, &escape, 1);
            zeros = 0;
        }
        g_byte_array_append(nal, buf + i, 1);
        zeros = buf[i] == 0 ? zeros + 1 : 0;
    }
    return nal;
}

static GByteArray *h264_pps(int chroma, bool cabac, bool weighted,
                             bool constrained, unsigned id)
{
    uint8_t config[64] = {0};
    unsigned bit = 0;
    h264_put(config, &bit, 0x68, 8);
    h264_ue(config, &bit, id); h264_ue(config, &bit, 0);
    h264_put(config, &bit, cabac, 1);
    h264_put(config, &bit, 0, 1); /* no bottom-field POC */
    h264_ue(config, &bit, 0); /* one slice group */
    h264_ue(config, &bit, 0); h264_ue(config, &bit, 0);
    h264_put(config, &bit, weighted, 1);
    h264_put(config, &bit, 0, 2); /* no B weighting */
    h264_se(config, &bit, 0); h264_se(config, &bit, 0);
    h264_se(config, &bit, chroma);
    /* Deblocking control present, constrained intra flag, no redundant count. */
    h264_put(config, &bit, 4 | (constrained << 1), 3);
    h264_put(config, &bit, 1, 1);
    return h264_escape(config, bit);
}

static bool h264_planes(IPodH264State *s, uint32_t slots, unsigned bytes,
                        uint32_t *y, uint32_t *uv)
{
    /* 0x106c and the L0 table contain Y/C address-register indices. */
    if (slots & ~0x7f7fu) {
        return false; /* field tags are not progressive frame addresses */
    }
    uint64_t ya = (uint64_t)s->regs[0x1200 / 4 + (slots & 127)] << 10;
    uint64_t ca = (uint64_t)s->regs[0x1200 / 4 + ((slots >> 8) & 127)] << 10;
    if (ya < 0x08000000 || ca < 0x08000000 || ya + bytes > 0x10000000 ||
        ca + bytes / 2 > 0x10000000) {
        return false;
    }
    *y = ya; *uv = ca;
    return true;
}

typedef struct H264References {
    unsigned count;
    unsigned indices[16];
    unsigned list[16];
} H264References;

static bool h264_references(IPodH264State *s, H264References *refs)
{
    uint32_t y[16], uv[16];
    unsigned active = s->regs[0x1040 / 4] + 1;
    unsigned bytes = s->regs[0x1030 / 4] * s->regs[0x1034 / 4] * 256;
    refs->count = 0;
    if (!active || active > 16) return false;
    for (unsigned i = 0; i < active; i++) {
        uint32_t ry, ruv;
        if (!h264_planes(s, s->regs[0x100 / 4 + i], bytes, &ry, &ruv)) return false;
        unsigned j;
        for (j = 0; j < refs->count; j++) {
            if (y[j] == ry && uv[j] == ruv) break;
        }
        refs->list[i] = j;
        if (j == refs->count) {
            y[j] = ry; uv[j] = ruv;
            refs->indices[j] = i;
            refs->count++;
        }
    }
    return true;
}

static bool h264_slice_nals(IPodH264State *s, GByteArray **sps,
                            GByteArray **pps, GByteArray **slice,
                            const H264References *reference_map,
                            unsigned *pcm_bit_offset)
{
    uint8_t config[64] = {0};
    unsigned bit = 0, width = s->regs[0x1030 / 4], height = s->regs[0x1034 / 4];
    int chroma = ((s->regs[0x1024 / 4] & 31) ^ 16) - 16;
    int qp = s->regs[0x1028 / 4];
    unsigned deblock = s->regs[0x105c / 4];
    int alpha = s->regs[0x1060 / 4], beta = s->regs[0x1064 / 4];
    unsigned type = s->regs[0x102c / 4], refs = s->regs[0x1040 / 4] + 1;
    unsigned cabac = s->regs[0x1020 / 4], cabac_init = s->regs[0x10cc / 4];
    unsigned weighted = s->regs[0x104c / 4];
    unsigned luma_denom = s->regs[0x1054 / 4], chroma_denom = s->regs[0x1058 / 4];
    /* Progressive I/P slices. B/field pictures need additional state mapped. */
    if (!width || !height || width > 128 || height > 128 ||
        (type != 0 && type != 2) || cabac > 1 || cabac_init > 2 ||
        (type == 0 && (!refs || refs > 16)) || weighted > 1 ||
        (type == 0 && weighted && (luma_denom > 7 || chroma_denom > 7)) ||
        s->regs[0x100c / 4] != 1 || s->regs[0x1010 / 4] ||
        s->regs[0x1018 / 4] > 1 || s->regs[0x1038 / 4] >= height ||
        s->regs[0x103c / 4] >= width ||
        s->regs[0x10d4 / 4] != 2 || qp < 0 || qp > 51 ||
        chroma < -12 || chroma > 12 || deblock > 2 ||
        alpha < -12 || alpha > 12 || beta < -12 || beta > 12 ||
        alpha % 2 || beta % 2 ||
        s->exhausted || s->bit >= s->rbsp->len * 8) {
        return false;
    }
    H264References references = {0};
    if (reference_map) references = *reference_map;
    else if (type == 0 && !h264_references(s, &references)) return false;
    bool idr = type == 2 && !references.count;
    h264_put(config, &bit, 0x67, 8);
    h264_put(config, &bit, (cabac || weighted) ? 77 : 66, 8); /* Main/Baseline */
    h264_put(config, &bit, (cabac || weighted) ? 0x40 : 0xc0, 8);
    h264_put(config, &bit, 51, 8);
    h264_ue(config, &bit, 0); /* SPS ID */
    h264_ue(config, &bit, 12); /* 16-bit frame_num */
    h264_ue(config, &bit, 2); /* decode-order POC */
    h264_ue(config, &bit, 16); /* hardware L0 references, seeded losslessly */
    h264_put(config, &bit, 0, 1);
    h264_ue(config, &bit, width - 1);
    h264_ue(config, &bit, height - 1);
    h264_put(config, &bit, 0xc, 4); /* frame-only, direct8x8, no crop/VUI */
    h264_put(config, &bit, 1, 1);
    *sps = h264_escape(config, bit);
    *pps = h264_pps(chroma, cabac, weighted, s->regs[0x1018 / 4], 0);
    g_autofree uint8_t *picture = g_malloc0(s->rbsp->len + 512);
    bit = 0;
    h264_put(picture, &bit, idr ? 0x65 : 0x41, 8);
    h264_ue(picture, &bit, s->regs[0x1038 / 4] * width + s->regs[0x103c / 4]);
    h264_ue(picture, &bit, type);
    h264_ue(picture, &bit, 0);
    h264_put(picture, &bit, idr ? 0 : references.count, 16);
    if (idr) h264_ue(picture, &bit, 0);
    if (type == 0) {
        h264_put(picture, &bit, 1, 1); /* override active L0 count */
        h264_ue(picture, &bit, refs - 1);
        /* Weighted prediction may repeat a picture in L0. Seeding duplicates
         * as different pictures changes deblocking reference identity even
         * though the pixel planes are identical. Reorder the unique DPB into
         * the guest's actual list, including repeated entries. */
        bool reordered = references.count != refs;
        for (unsigned i = 0; i < refs; i++) reordered |= references.list[i] != i;
        h264_put(picture, &bit, reordered, 1);
        if (reordered) {
            int previous = references.count;
            for (unsigned i = 0; i < refs; i++) {
                int next = references.count - 1 - references.list[i];
                int delta = previous - next;
                h264_ue(picture, &bit, delta < 0 ? 1 : 0);
                h264_ue(picture, &bit, delta ? abs(delta) - 1 : 65535);
                previous = next;
            }
            h264_ue(picture, &bit, 3);
        }
        if (weighted) {
            h264_ue(picture, &bit, luma_denom);
            h264_ue(picture, &bit, chroma_denom);
            for (unsigned i = 0; i < refs; i++) {
                uint32_t luma = s->regs[0x400 / 4 + i];
                uint32_t chroma = s->regs[0x480 / 4 + i];
                bool luma_default = luma == (1u << luma_denom);
                uint32_t cw = 1u << chroma_denom;
                bool chroma_default = chroma == (cw | (cw << 16));
                h264_put(picture, &bit, !luma_default, 1);
                if (!luma_default) {
                    h264_se(picture, &bit, (int8_t)luma);
                    h264_se(picture, &bit, (int8_t)(luma >> 8));
                }
                h264_put(picture, &bit, !chroma_default, 1);
                if (!chroma_default) {
                    for (unsigned ch = 0; ch < 2; ch++) {
                        h264_se(picture, &bit, (int8_t)(chroma >> (ch * 16)));
                        h264_se(picture, &bit, (int8_t)(chroma >> (ch * 16 + 8)));
                    }
                }
            }
        }
    }
    h264_put(picture, &bit, 0, idr ? 2 : 1); /* reference marking */
    if (cabac && type != 2) {
        h264_ue(picture, &bit, cabac_init);
    }
    h264_se(picture, &bit, qp - 26);
    h264_ue(picture, &bit, deblock);
    if (deblock != 1) {
        /* 7E18 c05b3778/c05b3788 expand the syntax offsets for hardware. */
        h264_se(picture, &bit, alpha / 2); h264_se(picture, &bit, beta / 2);
    }
    /* Retain the RBSP stop bit, replacing only its byte-alignment padding. */
    unsigned payload = s->bit, end = s->rbsp->len * 8;
    if (pcm_bit_offset) *pcm_bit_offset = cabac ? 0 : (payload - bit) & 7;
    if (cabac) {
        while (bit % 8) {
            h264_put(picture, &bit, 1, 1);
        }
        payload = (payload + 7) & ~7;
    }
    while (!cabac && end > s->bit && !(s->rbsp->data[(end - 1) / 8] & (1 << (7 - (end - 1) % 8)))) {
        end--;
    }
    for (unsigned i = payload; i < end; i++) {
        h264_put(picture, &bit, (s->rbsp->data[i / 8] >> (7 - i % 8)) & 1, 1);
    }
    *slice = h264_escape(picture, bit);
    return true;
}

static GByteArray *h264_reference_pixels(IPodH264State *s, uint32_t y,
                                        uint32_t uv, unsigned frame)
{
    unsigned width = s->regs[0x1030 / 4] * 16, height = s->regs[0x1034 / 4] * 16;
    unsigned bytes = width * height;
    g_autofree uint8_t *pixels = g_malloc(bytes * 3 / 2);
    if (address_space_read(&address_space_memory, y, MEMTXATTRS_UNSPECIFIED, pixels, bytes) ||
        address_space_read(&address_space_memory, uv, MEMTXATTRS_UNSPECIFIED,
                           pixels + bytes, bytes / 2)) {
        return NULL;
    }
    /* Native decoders hide their reference-picture buffers. Seed them with
     * lossless I_PCM pictures from the actual guest DMA planes, in L0 order.
     * This avoids an invented second DPB with stale guest ownership. P frames
     * need reference pixels only; B-direct motion vectors are not covered. */
    g_autofree uint8_t *picture = g_malloc0(bytes * 3 / 2 + bytes / 128 + 128);
    unsigned bit = 0;
    h264_put(picture, &bit, frame ? 0x41 : 0x65, 8);
    h264_ue(picture, &bit, 0); h264_ue(picture, &bit, 2);
    h264_ue(picture, &bit, s->regs[0x1020 / 4] ? 1 : 0);
    h264_put(picture, &bit, frame, 16);
    if (!frame) {
        h264_ue(picture, &bit, 0); h264_put(picture, &bit, 0, 2);
    } else {
        h264_put(picture, &bit, 0, 1);
    }
    h264_se(picture, &bit, 0); h264_ue(picture, &bit, 1); /* no filtering */
    for (unsigned row = 0; row < height; row += 16) {
        for (unsigned col = 0; col < width; col += 16) {
            h264_ue(picture, &bit, 25); /* I_PCM */
            bit = (bit + 7) & ~7;
            for (unsigned r = 0; r < 16; r++, bit += 128) {
                memcpy(picture + bit / 8, pixels + (row + r) * width + col, 16);
            }
            for (unsigned ch = 0; ch < 2; ch++) {
                for (unsigned r = 0; r < 8; r++) {
                    for (unsigned c = 0; c < 8; c++, bit += 8) {
                        picture[bit / 8] = pixels[bytes + (row / 2 + r) * width + col + c * 2 + ch];
                    }
                }
            }
        }
    }
    h264_put(picture, &bit, 1, 1);
    return h264_escape(picture, bit);
}

static GByteArray *h264_reference(IPodH264State *s, unsigned index, unsigned frame)
{
    uint32_t y, uv;
    unsigned bytes = s->regs[0x1030 / 4] * s->regs[0x1034 / 4] * 256;
    if (!h264_planes(s, s->regs[0x100 / 4 + index], bytes, &y, &uv)) return NULL;
    return h264_reference_pixels(s, y, uv, frame);
}

static bool h264_decode_native(IPodH264State *s)
{
#ifdef __APPLE__
    g_autoptr(GByteArray) sps = NULL, pps = NULL, slice = NULL;
    uint32_t y, uv;
    if (!h264_slice_nals(s, &sps, &pps, &slice, NULL, NULL) ||
        !h264_planes(s, s->regs[0x106c / 4],
            s->regs[0x1030 / 4] * s->regs[0x1034 / 4] * 256, &y, &uv)) {
        return false;
    }
    g_autoptr(GByteArray) seed_pps = h264_pps(0, false, false, false, 1);
    const uint8_t *sets[] = {sps->data, pps->data, seed_pps->data};
    size_t sizes[] = {sps->len, pps->len, seed_pps->len};
    CMVideoFormatDescriptionRef format = NULL;
    OSStatus err = CMVideoFormatDescriptionCreateFromH264ParameterSets(
        NULL, s->regs[0x1020 / 4] ? 3 : 2, sets, sizes, 4, &format);
    if (err) {
        return false;
    }
    /* The synthetic SPS has no VUI; matching its nominal limited range keeps
     * coded samples unchanged. The guest's compositor owns range conversion. */
    /* Reuse the native session when its parameter sets are unchanged. Each
     * job still starts with an IDR (real I picture or first lossless seed),
     * so guest DMA remains the sole authority for reference pixels. */
    if (!s->format || !CFEqual(s->format, format)) {
        h264_decoder_close(s);
        s->video = ipod_video_create(format,
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
        if (s->video) s->format = (CMVideoFormatDescriptionRef)CFRetain(format);
    }
    CFRelease(format);
    if (!s->video) {
        return false;
    }
    if (s->regs[0x102c / 4] == 0) {
        H264References refs;
        if (!h264_references(s, &refs)) return false;
        for (unsigned i = 0; i < refs.count; i++) {
            g_autoptr(GByteArray) ref = h264_reference(s, refs.indices[refs.count - 1 - i], i);
            if (!ref) {
                h264_decoder_close(s);
                return false;
            }
            uint8_t size[4]; stl_be_p(size, ref->len);
            g_byte_array_prepend(ref, size, 4);
            if (!ipod_video_frame(s->video, ref->data, ref->len, 0, 0)) {
                h264_decoder_close(s);
                return false;
            }
        }
    }
    uint8_t prefix[4]; stl_be_p(prefix, slice->len);
    g_byte_array_prepend(slice, prefix, 4);
    bool ok = ipod_video_frame(s->video, slice->data, slice->len, y, uv);
    if (!ok) h264_decoder_close(s);
    return ok;
#else
    return false;
#endif
}

#ifdef IT_HAVE_AVCODEC
/* One hardware job is one slice, while VideoToolbox requires a complete
 * picture. libavcodec's chunk mode retains a partial picture between jobs.
 * ponytail: commit DMA on picture completion. Replay only when a later slice
 * adds a reference; retain at most 64 MiB of slice data per picture. An external
 * reference-buffer backend would remove replay and allow per-slice DMA. */
static int h264_software_packet(IPodH264State *s, const GByteArray *nal,
                                unsigned pcm_bit_offset)
{
    if (av_opt_set_int(s->codec->priv_data, "cavlc_pcm_bit_offset",
                       pcm_bit_offset, 0) < 0) return -1;
    AVPacket *packet = av_packet_alloc();
    if (!packet || av_new_packet(packet, nal->len + 4) < 0) {
        av_packet_free(&packet);
        return -1;
    }
    memcpy(packet->data, "\0\0\0\1", 4);
    memcpy(packet->data + 4, nal->data, nal->len);
    int err = avcodec_send_packet(s->codec, packet);
    av_packet_free(&packet);
    if (err < 0) return -1;
    err = avcodec_receive_frame(s->codec, s->frame);
    if (err == AVERROR(EAGAIN)) return 0;
    if (err < 0 || (s->frame->flags & AV_FRAME_FLAG_CORRUPT) ||
        s->frame->decode_error_flags) return -1;
    return 1;
}

static GByteArray *h264_replay_slice(IPodH264State *s, H264SoftwareSlice *job,
                                    unsigned *pcm_bit_offset)
{
    IPodH264State saved = {.rbsp = job->rbsp, .bit = job->bit};
    H264References references = {.count = s->partial_reference_count};
    g_autoptr(GByteArray) sps = NULL, pps = NULL;
    GByteArray *slice = NULL;
    memcpy(saved.regs + 0x1000 / 4, job->regs, sizeof(job->regs));
    memcpy(saved.regs + 0x400 / 4, job->weights, 16 * sizeof(uint32_t));
    memcpy(saved.regs + 0x480 / 4, job->weights + 16, 16 * sizeof(uint32_t));
    memcpy(references.list, job->references, sizeof(references.list));
    if (!h264_slice_nals(&saved, &sps, &pps, &slice, &references,
                         pcm_bit_offset)) return NULL;
    return slice;
}

static bool h264_decode_software(IPodH264State *s)
{
    g_autoptr(GByteArray) sps = NULL, pps = NULL, slice = NULL;
    uint32_t y, uv, key[3] = {0};
    g_autofree uint8_t *pixels = NULL;
    unsigned pcm_bit_offset;
    unsigned mw = s->regs[0x1030 / 4], mh = s->regs[0x1034 / 4];
    if (!mw || mw > 128 || !mh || mh > 128) goto fail;
    unsigned width = mw * 16, height = mh * 16, bytes = width * height;
    unsigned first = s->regs[0x1038 / 4] * mw + s->regs[0x103c / 4];
    unsigned type = s->regs[0x102c / 4];
    unsigned refs = type == 0 ? s->regs[0x1040 / 4] + 1 : 0;
    if ((type != 0 && type != 2) || (type == 0 && (!refs || refs > 16))) goto fail;
    if (!h264_planes(s, s->regs[0x106c / 4], bytes, &y, &uv)) goto fail;
    key[0] = y; key[1] = uv;
    key[2] = mw | (mh << 8) | ((s->regs[0x1024 / 4] & 31) << 16) |
             (s->regs[0x1020 / 4] << 21) | (s->regs[0x104c / 4] << 22) |
             (s->regs[0x1018 / 4] << 23);
    if (first && (!s->partial || first <= s->partial_start ||
                  memcmp(key, s->partial_key, sizeof(key)))) goto fail;
    H264References references = {0};
    if (!first) {
        s->partial_reference_count = 0;
        s->partial_bytes = 0;
        if (!s->partial_slices) {
            s->partial_slices = g_ptr_array_new_with_free_func(h264_software_slice_free);
        }
        g_ptr_array_set_size(s->partial_slices, 0);
    }
    unsigned previous_count = s->partial_reference_count;
    for (unsigned i = 0; i < refs; i++) {
        uint32_t ry, ruv;
        if (!h264_planes(s, s->regs[0x100 / 4 + i], bytes, &ry, &ruv)) goto fail;
        unsigned j;
        for (j = 0; j < s->partial_reference_count; j++) {
            if (ry == s->partial_references[j][0] &&
                ruv == s->partial_references[j][1]) break;
        }
        if (j == s->partial_reference_count) {
            if (j == 16) goto fail;
            s->partial_references[j][0] = ry;
            s->partial_references[j][1] = ruv;
            s->partial_reference_count++;
        }
        references.list[i] = j;
    }
    references.count = s->partial_reference_count;
    if (!h264_slice_nals(s, &sps, &pps, &slice, &references,
                         &pcm_bit_offset)) goto fail;
    size_t retained = sizeof(H264SoftwareSlice) + s->rbsp->len;
    if (retained > 64 * 1024 * 1024 - s->partial_bytes) goto fail;
    H264SoftwareSlice *job = g_new0(H264SoftwareSlice, 1);
    memcpy(job->regs, s->regs + 0x1000 / 4, sizeof(job->regs));
    memcpy(job->weights, s->regs + 0x400 / 4, 16 * sizeof(uint32_t));
    memcpy(job->weights + 16, s->regs + 0x480 / 4, 16 * sizeof(uint32_t));
    memcpy(job->references, references.list, sizeof(job->references));
    job->bit = s->bit;
    job->rbsp = g_byte_array_sized_new(s->rbsp->len);
    g_byte_array_append(job->rbsp, s->rbsp->data, s->rbsp->len);
    g_ptr_array_add(s->partial_slices, job);
    s->partial_bytes += retained;
    if (!s->codec) {
        const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
        if (!codec) goto fail;
        s->codec = avcodec_alloc_context3(codec);
        s->frame = av_frame_alloc();
        if (!s->codec || !s->frame) goto fail;
        s->codec->thread_count = 1;
        s->codec->flags2 |= AV_CODEC_FLAG2_CHUNKS;
        s->codec->err_recognition = AV_EF_EXPLODE;
        /* Keep macroblock coverage checks. The packaged FFmpeg chunk fix
         * defers concealment until picture completion, not each slice. */
        if (av_opt_set_int(s->codec->priv_data, "enable_er", 1, 0) < 0 ||
            avcodec_open2(s->codec, codec, NULL) < 0) goto fail;
    }
    if (!first || references.count != previous_count) {
        avcodec_flush_buffers(s->codec);
        av_frame_unref(s->frame);
        memcpy(s->partial_key, key, sizeof(key));
        if (h264_software_packet(s, sps, 0) != 0 ||
            h264_software_packet(s, pps, 0) != 0) goto fail;
        if (s->regs[0x1020 / 4]) {
            g_autoptr(GByteArray) seed_pps = h264_pps(0, false, false, false, 1);
            if (h264_software_packet(s, seed_pps, 0) != 0) goto fail;
        }
        /* As in the native path, guest DMA owns every reference. Start each
         * picture with an IDR and seed the active L0 list from its real planes. */
        for (unsigned i = 0; i < references.count; i++) {
            unsigned index = references.count - 1 - i;
            g_autoptr(GByteArray) ref = h264_reference_pixels(s,
                s->partial_references[index][0], s->partial_references[index][1], i);
            if (!ref || h264_software_packet(s, ref, 0) != 1) goto fail;
            av_frame_unref(s->frame);
        }
        for (unsigned i = 0; i + 1 < s->partial_slices->len; i++) {
            unsigned replay_offset;
            g_autoptr(GByteArray) replay = h264_replay_slice(s,
                s->partial_slices->pdata[i], &replay_offset);
            if (!replay || h264_software_packet(s, replay, replay_offset) != 0) goto fail;
        }
    }
    int complete = h264_software_packet(s, slice, pcm_bit_offset);
    if (complete < 0) goto fail;
    s->partial = !complete;
    s->partial_start = first;
    if (!complete) return true;
    g_ptr_array_set_size(s->partial_slices, 0);
    s->partial_bytes = 0;
    AVFrame *frame = s->frame;
    if (frame->format != AV_PIX_FMT_YUV420P || frame->width != width ||
        frame->height != height || frame->linesize[0] < width ||
        frame->linesize[1] < width / 2 || frame->linesize[2] < width / 2) goto fail;
    pixels = g_malloc(bytes * 3 / 2);
    for (unsigned row = 0; row < height; row++) {
        memcpy(pixels + row * width, frame->data[0] + row * frame->linesize[0], width);
    }
    for (unsigned row = 0; row < height / 2; row++) {
        for (unsigned x = 0; x < width / 2; x++) {
            pixels[bytes + row * width + x * 2] = frame->data[1][row * frame->linesize[1] + x];
            pixels[bytes + row * width + x * 2 + 1] = frame->data[2][row * frame->linesize[2] + x];
        }
    }
    if (address_space_write(&address_space_memory, y, MEMTXATTRS_UNSPECIFIED, pixels, bytes) ||
        address_space_write(&address_space_memory, uv, MEMTXATTRS_UNSPECIFIED, pixels + bytes, bytes / 2)) goto fail;
    const char *dump_path = getenv("IT_VIDEO_DUMP");
    FILE *dump = dump_path ? fopen(dump_path, "ab") : NULL;
    if (dump) {
        if (fwrite(pixels, 1, bytes * 3 / 2, dump) != bytes * 3 / 2) {
            warn_report("H264: diagnostic frame dump failed");
        }
        fclose(dump);
    }
    av_frame_unref(frame);
    return true;
fail:
    h264_decoder_close(s);
    return false;
}
#endif

static bool h264_decode(IPodH264State *s)
{
#ifdef IT_HAVE_AVCODEC
    /* CAVLC PCM blocks retain their original bit alignment after header
     * replacement. VideoToolbox cannot accept that alignment metadata. */
    if (s->codec || !s->regs[0x1020 / 4]) return h264_decode_software(s);
#endif
    if (h264_decode_native(s)) return true;
#ifdef IT_HAVE_AVCODEC
    return h264_decode_software(s);
#else
    return false;
#endif
}

static uint64_t h264_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodH264State *s = opaque;
    uint32_t value = s->regs[addr / 4];
    if (addr >= 0x1400 && addr < 0x1480) {
        unsigned count = (addr - 0x1400) / 4;
        value = h264_bits(s, count ? count : 32);
    } else if (addr == 0x1480) {
        /* 7E18 c05b64ac peeks for rbsp_trailing_bits before optional PPS
         * extensions. The lookahead is zero-padded and never consumes data. */
        unsigned saved = s->bit;
        unsigned count = saved < s->rbsp->len * 8 ?
            MIN(32, s->rbsp->len * 8 - saved) : 0;
        value = count ? h264_bits(s, count) : 0;
        if (count && count < 32) value <<= 32 - count;
        s->bit = saved;
    }
    trace_ipod_touch_h264_read(addr, value);
    return value;
}

static void h264_write(void *opaque, hwaddr addr, uint64_t value, unsigned size)
{
    IPodH264State *s = opaque;
    trace_ipod_touch_h264_write(addr, value, s->bit);
    if (addr == 0x1074) {
        s->regs[addr / 4] &= ~value;
    } else {
        s->regs[addr / 4] = value;
    }
    if (addr == 0x1004) {
        /* 7E18 initializes the whole engine with 0x7ff. The 0x0c reset used
         * between NALs must preserve a picture assembled across slice jobs. */
        if ((value & 0x7ff) == 0x7ff) h264_decoder_close(s);
        s->regs[addr / 4] = 0; /* software reset bits self-clear */
    } else if (addr == 0x10c0 && value == 0) {
        h264_set_irq(s, false);
    } else if (addr == 0x1000 && value == 1) {
        bool ok = h264_decode(s);
        s->regs[0x1074 / 4] = ok ? 1 : 2;
        s->regs[0x1070 / 4] = ok ?
            ((s->regs[0x1034 / 4] - 1) << 16) | (s->regs[0x1030 / 4] - 1) : 0;
        s->regs[addr / 4] = 0;
        h264_set_irq(s, true);
    } else if (addr == 0x1600 && (value & 0x801) == 0x801) {
        h264_nal(s);
    } else if (addr == 0x1078 && (value == 1 || value == 3)) {
        /* Exp-Golomb look-ahead. c05b1bd8/c05b22a8 consume the reported
         * bits through the ordinary window, even for an inline result. */
        unsigned saved = s->bit, zeros = 0;
        while (zeros < 32 && !h264_bits(s, 1) && !s->exhausted) {
            zeros++;
        }
        if (s->exhausted || zeros == 32) {
            s->regs[0x107c / 4] = 0x80;
        } else if (zeros >= 12) {
            s->regs[0x107c / 4] = 0xc0 | zeros;
        } else {
            uint32_t code = (1u << zeros) - 1 + h264_bits(s, zeros);
            if (value == 3) {
                code = (code & 1) ? (code + 1) / 2 : -(code / 2);
            }
            s->regs[0x107c / 4] = (code << 8) | 0x80 | (zeros * 2 + 1);
        }
        s->bit = saved;
    }
}

static const MemoryRegionOps h264_ops = {
    .read = h264_read,
    .write = h264_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
};

static void h264_reset(DeviceState *dev)
{
    IPodH264State *s = (IPodH264State *)dev;
    h264_decoder_close(s);
    memset(s->regs, 0, sizeof(s->regs));
    g_byte_array_set_size(s->rbsp, 0);
    s->bit = 0;
    s->exhausted = false;
    h264_set_irq(s, false);
}

static void h264_init(Object *obj)
{
    IPodH264State *s = (IPodH264State *)obj;
    s->rbsp = g_byte_array_new();
    memory_region_init_io(&s->iomem, obj, &h264_ops, s, "h264", 0x4000);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    sysbus_init_irq(SYS_BUS_DEVICE(obj), &s->irq);
}

static void h264_finalize(Object *obj)
{
    h264_decoder_close((IPodH264State *)obj);
    g_byte_array_unref(((IPodH264State *)obj)->rbsp);
}

static int h264_put_rbsp(QEMUFile *f, void *pv, size_t size,
                         const VMStateField *field, JSONWriter *vmdesc)
{
    GByteArray *bytes = *(GByteArray **)pv;
    if (bytes->len > 4 * 1024 * 1024) return -E2BIG;
    qemu_put_be32(f, bytes->len);
    qemu_put_buffer(f, bytes->data, bytes->len);
    return qemu_file_get_error(f);
}

static int h264_get_rbsp(QEMUFile *f, void *pv, size_t size,
                         const VMStateField *field)
{
    GByteArray *bytes = *(GByteArray **)pv;
    uint32_t len = qemu_get_be32(f);
    if (len > 4 * 1024 * 1024) return -EINVAL;
    g_byte_array_set_size(bytes, len);
    if (qemu_get_buffer(f, bytes->data, len) != len) return -EIO;
    return qemu_file_get_error(f);
}

static const VMStateInfo vmstate_h264_rbsp = {
    .name = "h264-rbsp", .get = h264_get_rbsp, .put = h264_put_rbsp,
};

static int h264_post_load(void *opaque, int version_id)
{
    IPodH264State *s = opaque;
    if (s->bit > s->rbsp->len * 8) return -EINVAL;
    /* Host decode sessions and partial software pictures are recreated lazily.
     * The guest's bit reader and registers must retain their exact position. */
    h264_decoder_close(s);
    h264_set_irq(s, s->irq_level);
    return 0;
}

static const VMStateDescription h264_vmstate = {
    .name = "ipod-h264",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = h264_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, IPodH264State, 0x4000 / 4),
        VMSTATE_UINT32(bit, IPodH264State),
        VMSTATE_BOOL(exhausted, IPodH264State),
        VMSTATE_BOOL(irq_level, IPodH264State),
        VMSTATE_SINGLE(rbsp, IPodH264State, 1, vmstate_h264_rbsp, GByteArray *),
        VMSTATE_END_OF_LIST()
    },
};

static void h264_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    dc->vmsd = &h264_vmstate;
    device_class_set_legacy_reset(dc, h264_reset);
}

static const TypeInfo h264_type = {
    .name = "ipodtouch.h264",
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodH264State),
    .instance_init = h264_init,
    .instance_finalize = h264_finalize,
    .class_init = h264_class_init,
};

static void h264_register(void)
{
    type_register_static(&h264_type);
}
type_init(h264_register)
