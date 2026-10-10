/*
 * S5L8930 display: DisplayPipe0 (+ the RGBOUT pipe1 twin), CLCD timing
 * generators, dart2 and a QEMU console scanning out the UI layer.
 *
 * Register contract: docs/research/gap-display-stack-kernel.md §1.
 * The pipe is a RAM register file with three special things: the parameter
 * FIFO port (0x103c: header 0x8000_0000|nwords<<16|swapID, then packets of
 * count<<16|reg followed by `count` data words), the frame interrupt
 * (0x1028 enable / 0x102c W1C status, bit0 VBL + bit8 swap-done, completed
 * swap ID in 0x1048) and the UI layer registers the scanout reads
 * (0x1038 bit8/9 layer enable, 0x4044/0x5044 base, 0x4048 stride, 0x4040
 * format, 0x1030 size). Layer bases are dart2 IOVAs when the kernel has
 * installed a segment table for SID 0, physical addresses otherwise.
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/arm/s5l8930.h"
#include "hw/arm/frame-timeline.h"
#include "system/runstate.h"
#include "ui/input.h"
#include "qemu/cutils.h"
#include "system/address-spaces.h"
#include "migration/vmstate.h"
#include "ui/console.h"
#include "trace.h"
#include "hw/trace-printf.h"

#define PIPE_WORDS          (S5L8930_DISP_PIPE0_SIZE / 4)
#define CLCD_WORDS          (S5L8930_CLCD_SIZE / 4)
#define TVOUT_SIZE          0x2000

#define DP_FLAGS            0x101c    /* the kernel spins WHILE (v & 0xf0) == 0x20: read idle */
#define DP_IRQ_ENABLE       0x1028
#define DP_IRQ_STATUS       0x102c    /* W1C */
#define DP_SIZE             0x1030    /* w << 16 | h */
#define DP_LAYERS           0x1038    /* bit8 UI0, bit9 UI1 */
#define DP_FIFO_PORT        0x103c
#define DP_FIFO_COUNT       0x1044    /* polled to 0 */
#define DP_SWAP_DONE        0x1048    /* low 16 bits: last completed swap ID */
#define DP_UI_BASE(l)       (0x4000 + (l) * 0x1000)
#define DP_UI_FORMAT        0x40
#define DP_UI_ADDR          0x44
#define DP_UI_STRIDE        0x48
#define DP_UI_DST_ORIGIN    0x54      /* x << 16 | y on the panel */
#define DP_UI_SRC_SIZE      0x60      /* w << 16 | h of the source buffer */
#define DP_UI_DST_END       0x64      /* x1 << 16 | y1, the destination's far corner */

#define DP_IRQ_VBL          0x001
#define DP_IRQ_SWAP_DONE    0x100

#define CLCD_CTRL           0x00      /* bit8 = soft reset, self-clearing */
#define CLCD_ENVID          0x50      /* bit0 ENVID, bit1 "ready down" */
#define CLCD_SIZE           0x60      /* (w - 1) << 16 | (h - 1) */

#define VBL_PERIOD_NS       (NANOSECONDS_PER_SECOND / 60)
#define QUIET_RELATCH_VBLS  15        /* ~250 ms without a swap */

OBJECT_DECLARE_SIMPLE_TYPE(S5L8930DisplayState, S5L8930_DISPLAY)

typedef struct {
    uint32_t regs[PIPE_WORDS];
    qemu_irq irq;            /* the pipe's frame interrupt (DT clcd/rgbout interrupts[0]) */
    /* Parameter FIFO parser: packet in progress + the swap it belongs to. */
    uint32_t pkt_left;
    uint32_t pkt_off;
    uint32_t swap_id;
    bool swap_pending;       /* a swap arrived since the last VBL */
    uint32_t panel;          /* pipe0: w << 16 | h of a panel= override (issue #21), 0 = none */
    uint32_t native;         /* pipe0: w << 16 | h of the board's panel, the geometry iBoot programs */
} DisplayPipe;

/* A UI layer's scanout: its buffer and where it lands on the panel. */
typedef struct UILayer {
    uint32_t fmt, base, stride;
    unsigned sw, sh;                 /* source size */
    unsigned x0, y0, x1, y1;         /* destination rectangle, clipped to the panel */
} UILayer;

struct S5L8930DisplayState {
    SysBusDevice parent_obj;

    MemoryRegion pipe_mr[2], clcd_mr[2], dart_mr, tvout_mr;
    qemu_irq clcd_irq;
    QemuConsole *con;
    QEMUTimer *vbl;
    uint64_t fb_base;        /* property: seed iBoot's scanout when nonzero */
    uint16_t width, height;  /* properties: the board's panel, as iBoot programs it */
    uint32_t pw, ph;         /* "panel-width"/"panel-height": panel= override (issue #21), 0 = the board's */

    DisplayPipe pipe[2];     /* 0 = DisplayPipe0 (scanned out), 1 = RGBOUT */
    uint32_t clcd[2][CLCD_WORDS];
    uint32_t tvout[TVOUT_SIZE / 4];
    S5L8930Dart dart;        /* dart2 (hw/arm/s5l8930_dart.c) */

    /*
     * The frame the panel shows, latched at the VBL that completes a swap.
     * CoreAnimation renders into the layer's buffer in place (one page with
     * MBX2D_PAGE_FLIP=0) and starts the next frame as soon as the swap
     * completes, so reading the buffer live at host-refresh time caught it
     * half drawn: torn frames and black bands during every animation. Host
     * memory only; after a snapshot restore the next swap refills it.
     */
    uint8_t *front;
    size_t front_size;
    uint32_t front_key[4];   /* w, h, fmt, stride of the latched frame */
    bool front_valid;
    /*
     * What the console surface holds, so a refresh that has nothing new skips the 3 MiB copy and the
     * update (the panel's front buffer is redrawn only by a latch, and a quiet panel relatches every
     * VBL): front_gen counts latches that changed the picture; shown_* is what display_update drew.
     * Anything else (live layers before the first swap, a new surface, the panel going dark) draws.
     */
    uint8_t *front_next;     /* the relatch's scratch: kept only if it differs */
    /*
     * The layers' source rows the front was composed from (packed: the rows the panel shows), the
     * layers they were read as, and src_next, the next latch's read. A quiet panel relatches every
     * VBL, and composing the whole panel from guest memory 60 times a second was most of an idle A4's
     * host CPU (13% of a core at a still Home screen); unchanged sources skip the compose.
     */
    uint8_t *src, *src_next;
    size_t src_size, src_len;
    UILayer src_layers[2];
    uint64_t front_gen;
    uint64_t shown_gen;
    DisplaySurface *shown_surface;
    int shown;               /* 0 nothing known, 1 the latched front at shown_gen, 2 dark */
    unsigned quiet_vbls;     /* VBLs since the last swap */
    uint32_t swaps;          /* swaps latched since reset: frames the panel showed ("swaps") */
    FrameTimeline ftl;       /* per-vsync latched-frame ring in guest-virtual time */
    uint32_t stop_after;     /* "stop-after-vsyncs": pause the VM after this many more vsyncs (0 = off) */
    char vsync_input[32];    /* "vsync-input": one touch event delivered at the next vsync ("" = none) */
};

/* ---- DisplayPipe ------------------------------------------------------- */

static void pipe_update_irq(DisplayPipe *p)
{
    qemu_set_irq(p->irq, (p->regs[DP_IRQ_STATUS / 4] & p->regs[DP_IRQ_ENABLE / 4]) != 0);
}

static void pipes_update_irq(S5L8930DisplayState *s)
{
    pipe_update_irq(&s->pipe[0]);
    pipe_update_irq(&s->pipe[1]);
}

/*
 * A panel of another size (panel-width/-height, issue #21). iBoot programs
 * the timing of the panel its table knows (the board's width/height);
 * AppleDisplayPipe and
 * AppleCLCD adopt the geometry from these registers at start, so the words
 * carrying iBoot's native geometry read back as this panel's. Nothing else
 * is translated: what the kernel programs afterwards is already the panel's.
 */
static uint32_t panel_word(const DisplayPipe *p, hwaddr addr, uint32_t v)
{
    uint32_t panel = p->panel, native = p->native;
    unsigned w = panel >> 16, h = panel & 0xffff;
    unsigned nw = native >> 16, nh = native & 0xffff;

    if (!panel) {
        return v;
    }
    switch (addr) {
    case DP_SIZE:
    case DP_UI_BASE(0) + DP_UI_SRC_SIZE: case DP_UI_BASE(0) + DP_UI_DST_END:
    case DP_UI_BASE(1) + DP_UI_SRC_SIZE: case DP_UI_BASE(1) + DP_UI_DST_END:
        return v == native ? panel : v;
    case DP_UI_BASE(0) + DP_UI_STRIDE: case DP_UI_BASE(1) + DP_UI_STRIDE:
        return (v & ~0x3fu) == nw * 4 ? w * 4 | (v & 0x3f) : v;
    case CLCD_SIZE:
        return v == ((nw - 1) << 16 | (nh - 1)) ? (w - 1) << 16 | (h - 1) : v;
    default:
        return v;
    }
}

static uint64_t pipe_read(void *opaque, hwaddr addr, unsigned size)
{
    DisplayPipe *p = opaque;

    switch (addr) {
    case DP_FLAGS:
        return 0;
    case DP_FIFO_COUNT:
        return 0;
    case CLCD_SIZE:             /* not a pipe register */
        return p->regs[addr / 4];
    default:
        return panel_word(p, addr, p->regs[addr / 4]);
    }
}

static void pipe_fifo_write_word(DisplayPipe *p, uint32_t val);

static void pipe_fifo_write(DisplayPipe *p, uint32_t val)
{
    if (val & 0x80000000) {                     /* transaction header */
        p->swap_id = val & 0xffff;
        p->pkt_left = 0;
        p->swap_pending = true;
        return;
    }
    pipe_fifo_write_word(p, val);
}

static void pipe_fifo_write_word(DisplayPipe *p, uint32_t val)
{
    if (p->pkt_left == 0) {                     /* packet header */
        p->pkt_left = val >> 16;
        p->pkt_off = val & 0xffff;
    } else {                                    /* packet data */
        if (p->pkt_off < S5L8930_DISP_PIPE0_SIZE) {
            p->regs[p->pkt_off / 4] = val;
        }
        p->pkt_off += 4;
        p->pkt_left--;
    }
}

static void pipe_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    DisplayPipe *p = opaque;

    switch (addr) {
    case DP_FIFO_PORT:
        pipe_fifo_write(p, val);
        break;
    case DP_IRQ_STATUS:
        p->regs[addr / 4] &= ~val;
        break;
    case DP_FIFO_COUNT:
    case DP_SWAP_DONE:
        break;
    default:
        p->regs[addr / 4] = val;
    }
    pipe_update_irq(p);
}

static const MemoryRegionOps pipe_ops = {
    .read = pipe_read,
    .write = pipe_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void front_latch(S5L8930DisplayState *s);

/*
 * The frame end on one pipe: its queued swap completes (swap ID to 0x1048) and
 * VBL + swap-done latch in its status. Both pipes are the same DisplayPipe IP.
 * RGBOUT's matters at teardown: 4.3's QuartzCore swaps the H3RGBOutDisplay
 * once more as SpringBoard closes it at power-off, and IOMobileFramebuffer's
 * clientClose waits in swap_wait for that swap (8L1 0x8084e7b8, a
 * commandSleep until the swap ID completes). With no frame end on pipe1
 * the power-off slider hung there for good (smoke #27).
 * ponytail: both pipes run off the one 60 Hz tick, whatever RGBOUT's own
 * timing generator (0x89600000) is programmed to; model that block's timing
 * if a guest ever depends on a dock-video refresh rate.
 */
static void pipe_frame_end(DisplayPipe *p)
{
    uint32_t *r = p->regs;

    r[DP_SWAP_DONE / 4] = (r[DP_SWAP_DONE / 4] & ~0xffff) | p->swap_id;
    r[DP_IRQ_STATUS / 4] |= DP_IRQ_VBL | DP_IRQ_SWAP_DONE;
    pipe_update_irq(p);
}

/* One frame: the queued swaps complete and the VBLs fire. */
static void vbl_tick(void *opaque)
{
    S5L8930DisplayState *s = opaque;

    /* stop-after-vsyncs: the jank harness steps the machine one input sample per N vsyncs, so a gesture's
     * touch timing is fixed in virtual time instead of following the host's wall clock (docs/perf-jank.md). */
    if (s->vsync_input[0]) {
        /* "abs X Y" (0..32767) or "btn 0|1": the harness's touch sample, queued while the VM was paused and
         * delivered here, on the vsync, so its guest time is exact (input-send-event refuses a paused VM). */
        int x, y, down;
        if (sscanf(s->vsync_input, "abs %d %d", &x, &y) == 2) {
            qemu_input_queue_abs(NULL, INPUT_AXIS_X, x, 0, 32767);
            qemu_input_queue_abs(NULL, INPUT_AXIS_Y, y, 0, 32767);
        } else if (sscanf(s->vsync_input, "btn %d", &down) == 1) {
            qemu_input_queue_btn(NULL, INPUT_BUTTON_LEFT, down);
        }
        qemu_input_event_sync();
        s->vsync_input[0] = 0;
    }
    if (s->stop_after && --s->stop_after == 0) {
        qemu_system_vmstop_request_prepare();
        qemu_system_vmstop_request(RUN_STATE_PAUSED);
    }

    /* Measured with tests/ipad1/tearcheck.py: latching here left 5.2% bad
     * frames (6.6% on a rerun) against 10.4% for the live buffer, and 8.9%
     * when latched as the swap's last FIFO word arrived. */
    /*
     * IT_JANK_STALL_EVERY=N (test knob, like IT_VSYNC_DIVISOR): hold a pending
     * swap for one extra vsync every Nth stall opportunity, injecting a
     * deterministic ~33 ms hitch. Used only to prove the jank gate catches a
     * regression (tests/ipad1/jank.py --gate); unset in every real run.
     */
    static int stall_every = -1;
    if (stall_every < 0) {
        const char *v = getenv("IT_JANK_STALL_EVERY");
        stall_every = v ? atoi(v) : 0;
    }
    if (stall_every > 0 && s->pipe[0].swap_pending) {
        static unsigned opp;
        if (++opp % stall_every == 0) {
            /* Hold this swap one extra vsync: fire the VBL but not swap-done, so
             * the swap completes next tick -- a real 2-vsync panel stall the
             * guest blocks through in swap_wait. Records a held vsync. */
            frame_timeline_record(&s->ftl, 0, false);
            for (int i = 0; i < 2; i++) {
                s->pipe[i].regs[DP_IRQ_STATUS / 4] |= DP_IRQ_VBL;
                pipe_update_irq(&s->pipe[i]);
            }
            timer_mod(s->vbl, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VBL_PERIOD_NS);
            return;
        }
    }

    bool new_frame = s->pipe[0].swap_pending;
    /* One ring entry per vsync: a real swap is a new latched frame; every other
     * vsync held the same one (a dropped/duplicated frame for the jank metric).
     * ponytail: the Accessibility-Zoom relatch below writes new content without
     * a swap and is recorded as held; jank.py's canonical animations all swap,
     * so this under-counts only that one path -- key it off front-hash if a zoom
     * animation ever needs measuring. */
    frame_timeline_record(&s->ftl, s->pipe[0].swap_id, new_frame);
    if (s->pipe[0].swap_pending) {
        s->pipe[0].swap_pending = false;
        s->quiet_vbls = 0;
        s->swaps++;
        front_latch(s);
    } else if (++s->quiet_vbls >= QUIET_RELATCH_VBLS) {
        /*
         * No swap for a quarter second, yet the buffer can still change:
         * with Accessibility > Zoom on, CA stops swapping and the scaler
         * writes each magnified frame straight into the scanned-out buffer.
         * Real scanout reads memory live, so follow it: re-latch every VBL
         * while no swaps come. Swapping clients are latched on their swaps.
         */
        front_latch(s);
    }
    s->pipe[1].swap_pending = false;
    pipe_frame_end(&s->pipe[0]);
    pipe_frame_end(&s->pipe[1]);
    timer_mod(s->vbl, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VBL_PERIOD_NS);
}

/* ---- CLCD timing generator ---------------------------------------------- */

static uint64_t clcd_read(void *opaque, hwaddr addr, unsigned size)
{
    S5L8930DisplayState *s = opaque;
    uint32_t v = panel_word(&s->pipe[0], addr, s->clcd[0][addr / 4]);

    if (addr == CLCD_ENVID) {
        v = (v & ~2u) | ((v & 1) ? 0 : 2);
    }
    return v;
}

static void clcd_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    uint32_t *regs = ((S5L8930DisplayState *)opaque)->clcd[0];

    if (addr == CLCD_CTRL) {
        val &= ~0x100;    /* soft reset completes at once */
    }
    regs[addr / 4] = val;
}

/*
 * RGBOUT control (0x89600000, AppleRGBOUT reg index 1, this+0x1ab4) and
 * TV-out (SDO regs, this+0x5c): both power down by writing 0 to register
 * 0x00 and polling bit1 ("CLCK_DOWN_READY" c0627c6e, "SDO_CLKCON & 0x2"
 * c06757d6 in the 7B500 kernelcache). RGBOUT's reset writes 0x04 = 1 and
 * expects to read 1 back (c0627cf4), which plain RAM gives.
 */
static uint64_t ram_read(void *opaque, hwaddr addr, unsigned size)
{
    uint32_t v = ((uint32_t *)opaque)[addr / 4];

    if (addr == 0) {
        v = (v & ~2u) | ((v & 1) ? 0 : 2);
    }
    return v;
}

static void ram_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    ((uint32_t *)opaque)[addr / 4] = val;
}

static const MemoryRegionOps ram_ops = {
    .read = ram_read,
    .write = ram_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static const MemoryRegionOps clcd_ops = {
    .read = clcd_read,
    .write = clcd_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

/* ---- dart2 --------------------------------------------------------------- */

static hwaddr dart_xlate(S5L8930DisplayState *s, uint32_t va)
{
    return s5l8930_dart_xlate(&s->dart, 0, va);
}

/* dart2 for another client (DT dart-mapper reg: 1 RGBOUT, 2 scaler). */
hwaddr s5l8930_dart2_xlate(void *display, uint32_t va, unsigned sid)
{
    return s5l8930_dart_xlate(&S5L8930_DISPLAY(display)->dart, sid < S5L8930_DART_SIDS ? sid : 0, va);
}

/* Read `len` bytes of framebuffer at IOVA `va`, page by page. */
static void fb_read(S5L8930DisplayState *s, uint32_t va, uint8_t *dst, unsigned len)
{
    while (len) {
        unsigned n = MIN(len, 0x1000 - (va & 0xfff));
        hwaddr pa = dart_xlate(s, va);

        if (pa == (hwaddr)-1) {
            memset(dst, 0, n);
        } else {
            address_space_read(&address_space_memory, pa, MEMTXATTRS_UNSPECIFIED, dst, n);
        }
        va += n;
        dst += n;
        len -= n;
    }
}

/* ---- scanout ------------------------------------------------------------- */

static void display_invalidate(void *opaque)
{
    ((S5L8930DisplayState *)opaque)->shown = 0;     /* the next refresh draws */
}

/* Panel size: +0x1030, or the default before the kernel programs it. */
static void panel_size(S5L8930DisplayState *s, unsigned *w, unsigned *h)
{
    uint32_t v = panel_word(&s->pipe[0], DP_SIZE, s->pipe[0].regs[DP_SIZE / 4]);

    *w = (v >> 16) & 0x7ff;
    *h = v & 0x7ff;
    if (!*w || !*h) {
        *w = s->pw ? s->pw : s->width;
        *h = s->pw ? s->ph : s->height;
    }
}

/* UI layer `layer`'s scanout parameters, or false if it is off or unset. */
static bool scanout_layer(S5L8930DisplayState *s, int layer, unsigned w, unsigned h,
                          UILayer *u)
{
    uint32_t *r = s->pipe[0].regs, *ui = r + DP_UI_BASE(layer) / 4;
    uint32_t org = ui[DP_UI_DST_ORIGIN / 4], sz = ui[DP_UI_SRC_SIZE / 4];
    uint32_t end = ui[DP_UI_DST_END / 4];

    if (!(r[DP_LAYERS / 4] & (0x100 << layer))) {
        return false;
    }
    u->base = ui[DP_UI_ADDR / 4];
    u->fmt = (ui[DP_UI_FORMAT / 4] >> 8) & 7;
    /* Size and placement. A full-panel layer (and the boot surface, which
     * leaves them 0) covers the panel at 0,0; 4.x CA also places partial
     * layers, e.g. an EAGL surface as an overlay under its UI. */
    u->sw = sz ? sz >> 16 : w;
    u->sh = sz ? sz & 0xffff : h;
    /* ponytail: a source over 4096 pixels wide is not scanned out. It bounds the host's row buffers
     * (src) and no guest programs one; find the pipe's real limit if a guest ever does. */
    if (u->sw > 4096) {
        return false;
    }
    u->x0 = org >> 16;
    u->y0 = org & 0xffff;
    u->x1 = MIN(end ? end >> 16 : u->x0 + u->sw, w);
    u->y1 = MIN(end ? end & 0xffff : u->y0 + u->sh, h);
    if (!u->sw || !u->sh || u->x0 >= u->x1 || u->y0 >= u->y1) {
        return false;
    }
    /* Two encodings reach this register. The 7B500 swap path writes
     * (bytes per row << 4) | 2, e.g. (4096 << 4) | 2 for 1024x768 BGRA. The
     * iBoot framebuffer holds plain bytes per row: AppleDisplayPipe adopts it
     * with `stride = reg & ~0x3f` (0xc058c42a), so seeding the swap encoding
     * gave the boot surface 64 KiB rows. A <<4 value never undershoots a row
     * (of the layer's own width: an overlay is narrower than the panel). */
    {
        uint32_t v = ui[DP_UI_STRIDE / 4] & ~0x3fu;
        unsigned row = u->sw * (u->fmt ? 2 : 4);
        u->stride = (v >> 4) >= row ? v >> 4 : v;
    }
    return u->base != 0;
}

static size_t layer_row_bytes(const UILayer *u)
{
    return (size_t)u->sw * (u->fmt ? 2 : 4);
}

/*
 * Read the source row of layer `u` that each of its destination rows shows, packed into `dst`; the
 * bytes read. A layer neither scaled vertically nor padded is one read.
 */
static size_t layer_fetch(S5L8930DisplayState *s, const UILayer *u, uint8_t *dst)
{
    size_t row = layer_row_bytes(u);
    unsigned rows = u->y1 - u->y0;

    if (u->sh == rows && u->stride == row) {
        fb_read(s, u->base, dst, row * rows);
        return row * rows;
    }
    for (unsigned y = u->y0; y < u->y1; y++) {
        unsigned sy = (unsigned)((uint64_t)(y - u->y0) * u->sh / rows);
        fb_read(s, u->base + sy * u->stride, dst + (y - u->y0) * row, row);
    }
    return row * rows;
}

/* Fetch every shown layer into s->src_next, layer 0 first; the bytes. */
static size_t layers_fetch(S5L8930DisplayState *s, const UILayer u[2], const bool on[2])
{
    size_t need = 0, len = 0;

    for (int l = 0; l < 2; l++) {
        need += on[l] ? layer_row_bytes(&u[l]) * (u[l].y1 - u[l].y0) : 0;
    }
    if (need > s->src_size) {
        s->src = g_realloc(s->src, need);
        s->src_next = g_realloc(s->src_next, need);
        s->src_size = need;
    }
    for (int l = 0; l < 2; l++) {
        if (on[l]) {
            len += layer_fetch(s, &u[l], s->src_next + len);
        }
    }
    return len;
}

/* One fetched row of a layer as XRGB/ARGB8888 (0 ARGB/BGRA, 2 ARGB4444, 4 RGB565). */
static void layer_row(uint32_t fmt, const uint8_t *src, unsigned w, uint32_t *d)
{
    if (fmt == 0) {
        memcpy(d, src, w * 4);
        return;
    }
    for (unsigned x = 0; x < w; x++) {
        uint16_t v = lduw_le_p(src + x * 2);
        d[x] = fmt == 2
            ? ((uint32_t)(v & 0xf000) << 16 | (uint32_t)(v & 0xf000) << 12 |
               (v & 0xf00) << 12 | (v & 0xf00) << 8 |
               (v & 0x0f0) << 8 | (v & 0x0f0) << 4 |
               (v & 0x00f) << 4 | (v & 0x00f))
            : (0xff000000u | (v & 0xf800) << 8 | (v & 0xe000) << 3 |
               (v & 0x07e0) << 5 | (v & 0x0600) >> 1 |
               (v & 0x001f) << 3 | (v & 0x001c) >> 2);
    }
}

/*
 * The panel image: UI0, then UI1 over it. With GPU CoreAnimation both are on
 * (0x1038 = 0x300): UI0 keeps the boot surface and CA double-buffers the
 * whole screen in UI1 (0x603000 / 0x904000), each with its own base, stride
 * and format. UI1 is blended source-over with its alpha (premultiplied, as CA
 * renders); an opaque UI1 simply replaces UI0. Software CA uses UI0 alone.
 * Each layer covers its destination rectangle (+0x54 origin .. +0x64 far
 * corner) with its source (+0x60 size), nearest-neighbor if the two differ,
 * and is transparent outside it. 4.x CA puts an EAGL layer's surface in UI0
 * that way, under a full-panel UI1 that is clear where the layer shows.
 */
static bool layers(S5L8930DisplayState *s, unsigned w, unsigned h, UILayer u[2], bool on[2],
                   uint32_t key[4])
{
    memset(u, 0, 2 * sizeof(*u));
    for (int l = 0; l < 2; l++) {
        on[l] = scanout_layer(s, l, w, h, &u[l]);
    }
    if (key) {
        key[0] = w << 16 | h;
        key[1] = (on[0] ? 1 : 0) | (on[1] ? 2 : 0);
        key[2] = on[0] ? u[0].base : 0;
        key[3] = on[1] ? u[1].base : 0;
    }
    return on[0] || on[1];
}

/*
 * The video layer (0x1038 bit 10; block at +0x3000): NV12 that the pipe scales and converts itself, as
 * 4.x plays a movie (MPMoviePlayer: VXD decodes, the M2 scaler turns the frame upright, the pipe shows
 * it under UI1, which CA leaves clear there). +0x307c/+0x3080 Y and CbCr bases, +0x3088/+0x308c their
 * strides, +0x3094 source w << 16 | h, +0x309c destination size, +0x30a0 destination origin,
 * +0x3024..+0x3044 the YCbCr-to-RGB matrix (signed 4.12, rows R, G, B; columns Y, Cb, Cr).
 * ponytail: nearest sampling where the pipe has polyphase taps (+0x3120 on); the blend order taken as
 * video under UI1 (+0x2044/+0x2048); low stride bits (+0x3088 = 0x142) dropped as flags.
 */
static bool video_compose(S5L8930DisplayState *s, unsigned w, unsigned h, uint32_t *out)
{
    uint32_t *r = s->pipe[0].regs;
    unsigned sw = (r[0x3094 / 4] >> 16) & 0xfff, sh = r[0x3094 / 4] & 0xfff;
    unsigned dw = (r[0x309c / 4] >> 16) & 0xfff, dh = r[0x309c / 4] & 0xfff;
    unsigned x0 = r[0x30a0 / 4] >> 16, y0 = r[0x30a0 / 4] & 0xffff;
    unsigned ys = r[0x3088 / 4] & ~0xfu, uvs = r[0x308c / 4] & ~0xfu;
    int m[9];

    if (!(r[DP_LAYERS / 4] & 0x400) || !sw || !sh || !dw || !dh || sw > 4096 || sh > 4096 ||
        ys < sw || uvs < sw || (sw | sh) & 1) {
        return false;
    }
    for (int i = 0; i < 9; i++) {
        m[i] = (int16_t)r[0x3024 / 4 + i];
    }
    g_autofree uint8_t *yp = g_malloc((size_t)sw * sh), *uvp = g_malloc((size_t)sw * sh / 2);
    for (unsigned y = 0; y < sh; y++) {
        fb_read(s, r[0x307c / 4] + y * ys, yp + (size_t)y * sw, sw);
        if (!(y & 1)) {
            fb_read(s, r[0x3080 / 4] + y / 2 * uvs, uvp + (size_t)y / 2 * sw, sw);
        }
    }
    memset(out, 0, (size_t)w * h * 4);
    for (unsigned y = y0; y < MIN(y0 + dh, h); y++) {
        unsigned sy = (unsigned)((uint64_t)(y - y0) * sh / dh);
        for (unsigned x = x0; x < MIN(x0 + dw, w); x++) {
            unsigned sx = (unsigned)((uint64_t)(x - x0) * sw / dw);
            const uint8_t *c = uvp + (size_t)(sy / 2) * sw + (sx & ~1u);
            int in[3] = { yp[(size_t)sy * sw + sx] - 16, c[0] - 128, c[1] - 128 };
            uint32_t px = 0xff000000u;
            for (int k = 0; k < 3; k++) {
                int v = (m[k * 3] * in[0] + m[k * 3 + 1] * in[1] + m[k * 3 + 2] * in[2] + 2048) >> 12;
                px |= (uint32_t)MIN(255, MAX(0, v)) << (16 - 8 * k);    /* BGRA: R in bits 16-23 */
            }
            out[(size_t)y * w + x] = px;
        }
    }
    return true;
}

/* The panel image from the layers' fetched rows (layers_fetch). */
static void blend(const UILayer u[2], const bool on[2], const uint8_t *src, unsigned w,
                  unsigned h, uint32_t *out, const uint32_t *base)
{
    const uint8_t *lsrc[2];
    g_autofree uint32_t *row = g_new(uint32_t, MAX(MAX(on[0] ? u[0].sw : 0, on[1] ? u[1].sw : 0), w));

    lsrc[0] = src;
    lsrc[1] = src + (on[0] ? layer_row_bytes(&u[0]) * (u[0].y1 - u[0].y0) : 0);
    for (unsigned y = 0; y < h; y++) {
        uint32_t *d = out + (size_t)y * w;

        if (base) {
            memcpy(d, base + (size_t)y * w, w * 4);
        } else {
            memset(d, 0, w * 4);
        }
        for (int l = 0; l < 2; l++) {
            const UILayer *L = &u[l];
            unsigned dw = L->x1 - L->x0;

            if (!on[l] || y < L->y0 || y >= L->y1) {
                continue;
            }
            /* Unscaled BGRA that replaces what is under it: the row as it is. */
            if ((l == 0 || !on[0]) && !base && L->fmt == 0 && L->sw == dw) {
                memcpy(d + L->x0, lsrc[l] + (y - L->y0) * layer_row_bytes(L), dw * 4);
                continue;
            }
            layer_row(L->fmt, lsrc[l] + (y - L->y0) * layer_row_bytes(L), L->sw, row);
            for (unsigned x = L->x0; x < L->x1; x++) {
                unsigned sx = L->sw == dw ? x - L->x0
                            : (unsigned)((uint64_t)(x - L->x0) * L->sw / dw);
                uint32_t t = row[sx], a = t >> 24;

                if ((l == 0 && !base) || a == 0xff || (!on[0] && !base)) {
                    d[x] = t;
                } else if (a) {
                    uint32_t b = d[x], ia = 255 - a, o = 0xff000000u;
                    for (int sh = 0; sh < 24; sh += 8) {
                        uint32_t c = ((t >> sh) & 0xff) + (((b >> sh) & 0xff) * ia + 127) / 255;
                        o |= MIN(c, 255u) << sh;
                    }
                    d[x] = o;
                }
            }
        }
    }
}

static bool compose(S5L8930DisplayState *s, unsigned w, unsigned h, uint32_t *out,
                    uint32_t key[4])
{
    UILayer u[2];
    bool on[2];

    if (!layers(s, w, h, u, on, key)) {
        return false;
    }
    if (out) {
        g_autofree uint32_t *base = g_new(uint32_t, (size_t)w * h);
        bool video = video_compose(s, w, h, base);
        layers_fetch(s, u, on);
        blend(u, on, s->src_next, w, h, out, video ? base : NULL);
    }
    return true;
}
static void front_latch(S5L8930DisplayState *s)
{
    unsigned w, h;
    size_t need;

    panel_size(s, &w, &h);
    if (trace_event_get_state_backends(TRACE_S5L8930_DISPLAY_LOG)) {
        static unsigned n;
        uint32_t *r = s->pipe[0].regs;
        if (n++ < 200) {
            TRACE_PRINTF(trace_s5l8930_display_log, "[disp] swap %u: layers=%08x %ux%u "
                    "(raw %08x/%08x) ui0=%08x ui1=%08x\n", s->pipe[0].swap_id,
                    r[DP_LAYERS / 4], w, h,
                    r[(DP_UI_BASE(0) + DP_UI_STRIDE) / 4], r[(DP_UI_BASE(1) + DP_UI_STRIDE) / 4],
                    r[(DP_UI_BASE(0) + DP_UI_ADDR) / 4], r[(DP_UI_BASE(1) + DP_UI_ADDR) / 4]);
        }
    }
    need = (size_t)w * h * 4;
    if (need > s->front_size) {
        s->front = g_realloc(s->front, need);
        s->front_next = g_realloc(s->front_next, need);
        s->front_size = need;
    }
    uint32_t key[4];
    UILayer u[2];
    bool on[2];
    bool valid = layers(s, w, h, u, on, key);
    size_t len = valid ? layers_fetch(s, u, on) : 0;
    g_autofree uint32_t *base = g_new(uint32_t, (size_t)w * h);
    bool video = valid && video_compose(s, w, h, base);

    /* The front was composed from these very layers and bytes. */
    if (valid && !video && valid == s->front_valid && !memcmp(key, s->front_key, sizeof(key)) &&
        len == s->src_len && !memcmp(u, s->src_layers, sizeof(u)) &&
        !memcmp(s->src_next, s->src, len)) {
        return;
    }
    if (valid) {
        uint8_t *t = s->src;
        s->src = s->src_next;
        s->src_next = t;
        s->src_len = len;
        memcpy(s->src_layers, u, sizeof(u));
        blend(u, on, s->src, w, h, (uint32_t *)s->front_next, video ? base : NULL);
    }
    /* Swap in the new picture only when it differs: an unchanged relatch leaves front_gen alone. */
    if (valid != s->front_valid || memcmp(key, s->front_key, sizeof(key)) ||
        (valid && memcmp(s->front_next, s->front, need))) {
        uint8_t *t = s->front;
        s->front = s->front_next;
        s->front_next = t;
        memcpy(s->front_key, key, sizeof(key));
        s->front_valid = valid;
        s->front_gen++;
    }
}

/* ponytail: full redraw every host refresh (~30 Hz, 3 MiB), add dirty
 * tracking via framebuffer_update_display if it shows up in profiles. */
static void display_update(void *opaque)
{
    S5L8930DisplayState *s = opaque;
    DisplaySurface *surface;
    unsigned w, h;
    uint32_t key[4];
    g_autofree uint32_t *live = NULL;
    const uint32_t *img;
    bool lit;

    panel_size(s, &w, &h);
    surface = qemu_console_surface(s->con);
    if (surface_width(surface) != w || surface_height(surface) != h) {
        qemu_console_resize(s->con, w, h);
        surface = qemu_console_surface(s->con);
        s->shown = 0;
    }
    if (surface_bits_per_pixel(surface) != 32) {
        return;
    }

    /* The frame completed by the last swap, if it still describes these
     * layers; the live buffers only before the first swap (iBoot, early boot). */
    lit = compose(s, w, h, NULL, key);
    if (lit && s->front_valid && !memcmp(key, s->front_key, sizeof(key))) {
        img = (const uint32_t *)s->front;
        if (s->shown == 1 && s->shown_gen == s->front_gen && s->shown_surface == surface) {
            return;     /* the surface already holds this latched frame */
        }
        s->shown = 1;
        s->shown_gen = s->front_gen;
    } else if (lit) {
        live = g_new(uint32_t, (size_t)w * h);
        compose(s, w, h, live, NULL);
        img = live;
    } else {
        img = NULL;     /* no layer: the panel is off (ApplePinotLCD _lcdEnable 0) */
    }
    if (!img) {
        if (s->shown == 2 && s->shown_surface == surface) {
            return;     /* still dark */
        }
        s->shown = 2;
    } else if (img == live) {
        s->shown = 0;   /* live layers: drawn every refresh, as they may change without a swap */
    }
    s->shown_surface = surface;
    for (unsigned y = 0; y < h; y++) {
        uint8_t *d = surface_data(surface) + y * surface_stride(surface);

        if (img) {
            memcpy(d, img + (size_t)y * w, w * 4);
        } else {
            memset(d, 0, w * 4);
        }
    }
    dpy_gfx_update(s->con, 0, 0, w, h);
}

static const GraphicHwOps display_gfx_ops = {
    .invalidate = display_invalidate,
    .gfx_update = display_update,
};

/* ---- device -------------------------------------------------------------- */

static void s5l8930_display_reset(DeviceState *dev)
{
    S5L8930DisplayState *s = S5L8930_DISPLAY(dev);
    uint32_t *r = s->pipe[0].regs;

    for (int i = 0; i < 2; i++) {
        memset(s->pipe[i].regs, 0, sizeof(s->pipe[i].regs));
        s->pipe[i].pkt_left = s->pipe[i].pkt_off = s->pipe[i].swap_id = 0;
        s->pipe[i].swap_pending = false;
    }
    memset(s->clcd, 0, sizeof(s->clcd));
    memset(s->tvout, 0, sizeof(s->tvout));
    memset(&s->dart, 0, sizeof(s->dart));
    s->front_valid = false;
    s->shown = 0;
    s->swaps = 0;
    frame_timeline_reset(&s->ftl);

    /* What iBoot leaves behind: UI0 live on a panel-sized 32bpp buffer. The
     * kernel adopts it from these registers, so without them there is no
     * console framebuffer. */
    r[DP_SIZE / 4] = s->width << 16 | s->height;
    /*
     * And the CLCD timing iBoot programs from its "k48" display-timing entry
     * (iBoot-931 5ff01e0e-5ff01eaa, table 5ff2b028: 1024x768, 68.4 MHz,
     * h 133/133/135, v 10/10/12, 18 bpp): fields are value - 1. 4.x's
     * AppleCLCD::start_hardware (8C148 809d7e42) refuses a panel whose
     * vertical timing word 0x58 is zero and never registers a framebuffer.
     */
    s->clcd[0][0x00 / 4] = 0x4;
    s->clcd[0][0x04 / 4] = 0x3;
    s->clcd[0][0x14 / 4] = 0x81110001;          /* 0x80000001 | 0x1110000: <= 18 bpp */
    s->clcd[0][0x18 / 4] = 0x20408;
    s->clcd[0][0x58 / 4] = 9 << 16 | 9 << 8 | 11;
    s->clcd[0][0x5c / 4] = 132 << 16 | 132 << 8 | 134;
    s->clcd[0][0x60 / 4] = (s->width - 1) << 16 | (s->height - 1);
    /*
     * And the timing generator running (ENVID): iBoot hands over a lit panel. 5.x's AppleCLCD::start_hardware
     * (9A334 0x8084b9a0) adopts the display only when CLCD +0x50 bit0 is set; otherwise it resets the pipe and
     * returns false without a log line, so no framebuffer and the boot logo stays up. 3.x/4.x never read it.
     */
    s->clcd[0][CLCD_ENVID / 4] = 1;
    if (s->fb_base) {
        r[DP_LAYERS / 4] = 0x100;
        r[(DP_UI_BASE(0) + DP_UI_ADDR) / 4] = s->fb_base;
        /* Plain bytes per row. This was (4096 << 4) | 2, which the kernel
         * adopted as a 64 KiB row: a 48 MiB default surface (0x3000000,
         * too big for PurpleGfxMem, so a buffer instead) whose black fill
         * at power-off (CA fill_iosurface, CGBlt_fillBytes) ran off its
         * mapping. SpringBoard died with SIGBUS (KERN_PROTECTION_FAILURE)
         * and never reached reboot2, so Hold -> slide never powered off. */
        r[(DP_UI_BASE(0) + DP_UI_STRIDE) / 4] = s->width * 4 | 2;
        r[0x4060 / 4] = s->width << 16 | s->height;
    }
    pipes_update_irq(s);
    timer_mod(s->vbl, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + VBL_PERIOD_NS);
}

static void s5l8930_display_realize(DeviceState *dev, Error **errp)
{
    S5L8930DisplayState *s = S5L8930_DISPLAY(dev);

    s->vbl = timer_new_ns(QEMU_CLOCK_VIRTUAL, vbl_tick, s);
    s->con = graphic_console_init(dev, 0, &display_gfx_ops, s);
    s->pipe[0].native = s->width << 16 | s->height;
    if (s->pw || s->ph) {
        if (s->pw < 64 || s->ph < 64 || s->pw > 2047 || s->ph > 2047) {
            error_setg(errp, "panel must be 64..2047 pixels each way");
            return;
        }
        if (s->pw != s->width || s->ph != s->height) {
            s->pipe[0].panel = s->pw << 16 | s->ph;
        }
    }
    qemu_console_resize(s->con, s->pw ? s->pw : s->width, s->pw ? s->ph : s->height);
}

static char *s5l8930_get_frame_timeline(Object *obj, Error **errp)
{
    return frame_timeline_dump(&S5L8930_DISPLAY(obj)->ftl);
}

static void s5l8930_set_vsync_input(Object *obj, const char *v, Error **errp)
{
    S5L8930DisplayState *s = S5L8930_DISPLAY(obj);
    if (strlen(v) >= sizeof(s->vsync_input)) {
        error_setg(errp, "vsync-input: 'abs X Y' or 'btn 0|1'");
        return;
    }
    pstrcpy(s->vsync_input, sizeof(s->vsync_input), v);
}

static void s5l8930_display_init(Object *obj)
{
    S5L8930DisplayState *s = S5L8930_DISPLAY(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /* Order matches the header: pipe0, CLCD, DART2, RGBOUT (= AppleRGBOUT's
     * own DisplayPipe, reg index 0), TVOUT, RGBOUT2 (its control block). */
    memory_region_init_io(&s->pipe_mr[0], obj, &pipe_ops, &s->pipe[0],
                          "s5l8930.disp.pipe0", S5L8930_DISP_PIPE0_SIZE);
    memory_region_init_io(&s->clcd_mr[0], obj, &clcd_ops, s,
                          "s5l8930.disp.clcd", S5L8930_CLCD_SIZE);
    memory_region_init_io(&s->dart_mr, obj, &s5l8930_dart_ops, &s->dart,
                          "s5l8930.disp.dart2", S5L8930_DART_SIZE);
    memory_region_init_io(&s->pipe_mr[1], obj, &pipe_ops, &s->pipe[1],
                          "s5l8930.disp.rgbout", S5L8930_DISP_PIPE0_SIZE);
    memory_region_init_io(&s->tvout_mr, obj, &ram_ops, s->tvout,
                          "s5l8930.disp.tvout", TVOUT_SIZE);
    memory_region_init_io(&s->clcd_mr[1], obj, &ram_ops, s->clcd[1],
                          "s5l8930.disp.rgbout2", S5L8930_CLCD_SIZE);
    sysbus_init_mmio(sbd, &s->pipe_mr[0]);
    sysbus_init_mmio(sbd, &s->clcd_mr[0]);
    sysbus_init_mmio(sbd, &s->dart_mr);
    sysbus_init_mmio(sbd, &s->pipe_mr[1]);
    sysbus_init_mmio(sbd, &s->tvout_mr);
    sysbus_init_mmio(sbd, &s->clcd_mr[1]);
    sysbus_init_irq(sbd, &s->pipe[0].irq);
    sysbus_init_irq(sbd, &s->clcd_irq);
    sysbus_init_irq(sbd, &s->pipe[1].irq);
    object_property_add_uint32_ptr(obj, "swaps", &s->swaps, OBJ_PROP_FLAG_READ);
    object_property_add_str(obj, "frame-timeline", s5l8930_get_frame_timeline, NULL);
    object_property_add_uint32_ptr(obj, "stop-after-vsyncs", &s->stop_after, OBJ_PROP_FLAG_READWRITE);
    object_property_add_str(obj, "vsync-input", NULL, s5l8930_set_vsync_input);
    object_property_set_description(obj, "frame-timeline",
        "Latched-frame ring, one 'seq virt_ns newframe key' line per vsync in "
        "guest-virtual ns; the jank harness reads it (docs/perf-jank.md)");
}

static int s5l8930_display_post_load(void *opaque, int version_id)
{
    /* The latched frame is host memory and was not saved: take it again
     * from the restored guest RAM so the panel resumes on a whole frame. */
    front_latch(opaque);
    pipes_update_irq(opaque);
    return 0;
}

static bool display_pipe_swap_needed(void *opaque)
{
    return ((DisplayPipe *)opaque)->swap_pending;
}

/* A swap in flight when the state was saved: its VBL still owes the latch.
 * A subsection, so saves taken with nothing pending stay loadable by older
 * builds. */
static const VMStateDescription vmstate_display_pipe_swap = {
    .name = "s5l8930.display.pipe/swap",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = display_pipe_swap_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(swap_pending, DisplayPipe),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_display_pipe = {
    .name = "s5l8930.display.pipe",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(regs, DisplayPipe, PIPE_WORDS),
        VMSTATE_UINT32(pkt_left, DisplayPipe),
        VMSTATE_UINT32(pkt_off, DisplayPipe),
        VMSTATE_UINT32(swap_id, DisplayPipe),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_display_pipe_swap,
        NULL
    }
};

static const VMStateDescription vmstate_s5l8930_display = {
    .name = "s5l8930.display",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = s5l8930_display_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(pipe, S5L8930DisplayState, 2, 1,
                             vmstate_display_pipe, DisplayPipe),
        VMSTATE_UINT32_2DARRAY(clcd, S5L8930DisplayState, 2, CLCD_WORDS),
        VMSTATE_UINT32_ARRAY(tvout, S5L8930DisplayState, TVOUT_SIZE / 4),
        VMSTATE_UINT32_ARRAY(dart.regs, S5L8930DisplayState, S5L8930_DART_SIZE / 4),
        VMSTATE_UINT32_2DARRAY(dart.ste, S5L8930DisplayState, S5L8930_DART_SIDS, S5L8930_DART_SEGS),
        VMSTATE_TIMER_PTR(vbl, S5L8930DisplayState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property s5l8930_display_properties[] = {
    DEFINE_PROP_UINT64("fb-base", S5L8930DisplayState, fb_base, 0),
    DEFINE_PROP_UINT16("width", S5L8930DisplayState, width, 1024),     /* K48 */
    DEFINE_PROP_UINT16("height", S5L8930DisplayState, height, 768),
    DEFINE_PROP_UINT32("panel-width", S5L8930DisplayState, pw, 0),     /* unset: width */
    DEFINE_PROP_UINT32("panel-height", S5L8930DisplayState, ph, 0),
};

static void s5l8930_display_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = s5l8930_display_realize;
    device_class_set_legacy_reset(dc, s5l8930_display_reset);
    device_class_set_props(dc, s5l8930_display_properties);
    dc->vmsd = &vmstate_s5l8930_display;
}

static const TypeInfo s5l8930_display_info = {
    .name          = TYPE_S5L8930_DISPLAY,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(S5L8930DisplayState),
    .instance_init = s5l8930_display_init,
    .class_init    = s5l8930_display_class_init,
};

static void s5l8930_display_register_types(void)
{
    type_register_static(&s5l8930_display_info);
}

type_init(s5l8930_display_register_types)
