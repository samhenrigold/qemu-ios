/* S5L8930 H2FMI (hw/arm/s5l8930_h2fmi.c): a chip's array operations run off the BQL and end later, as tR, tPROG
 * and tBERS do on a chip, so the MMIO write that commands one returns while the store's I/O is still in flight.
 *
 * Driven through the MMIO handlers with the IOP page store stubbed: s5l8930_iop_nand_async only records each request,
 * and the test ends them when it chooses, as the block layer's completion does in the main loop. Pinned:
 *   - The iPhone 5 IOP firmware's FMISS page read (0x0a/0x37, status 0x77/0x7d, wait 0a004040, DATA_OUT 0x7a, four
 *     DMA tasks, as a boot trace shows it): the write that queues the read returns with the read in flight; the
 *     sequencer waits at its status poll, which reads busy (0); the read's end resumes it, the status reads 0x40 and
 *     the page reaches the FIFOs; DATA_OUT takes the page register's copy, one store read per page.
 *   - The A4 firmware's and iBoot's page read: an FMC status poll (go 0x40) and a transfer (control 3) issued while
 *     the chip reads wait for it, and its end raises the poll's event, DONE and the data.
 *   - A program (0x10) reads busy until it ends, its page register free at once for the next page; its failure shows
 *     in the status after the poll.
 *   - A chip runs its operations in order, one at a time; a reset drops the ones not started and waits for the rest.
 * With the change reverted the model calls the store synchronously from the MMIO handler: this test then fails
 * (it cuts h2fmi_busy, h2fmi_op_queue and h2fmi_quiesce, which are not there, and refuses a synchronous store call).
 *
 * SLICE include/hw/arm/s5l8930.h range typedef enum { S5L8930_NAND_READ | void s5l8930_iop_nand_async(
 * SLICE hw/arm/s5l8930_h2fmi.c range #define H2FMI_BUSES | #define HT(
 * SLICE hw/arm/s5l8930_h2fmi.c fn h2fmi_format h2fmi_status h2fmi_update_irq h2fmi_ce h2fmi_fmc_events h2fmi_busy h2fmi_ppn_status h2fmi_op_new h2fmi_op_queue h2fmi_op_done h2fmi_command h2fmi_go fifo_compact h2fmi_raw_read h2fmi_read_bytes h2fmi_room h2fmi_transfer h2fmi_meta_per_page h2fmi_seq_task h2fmi_seq_run h2fmi_write_check h2fmi_drain fifo_pop h2fmi_ecc_sector h2fmi_read h2fmi_write h2fmi_quiesce
 */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define trace_event_get_state_backends(id) 0
#define TRACE_PRINTF(fn, ...) do { if (0) printf(__VA_ARGS__); } while (0)
typedef uint64_t hwaddr;
typedef int SysBusDevice, MemoryRegion, qemu_irq;
typedef struct DeviceState DeviceState;
typedef struct S5L8930H2FMIState S5L8930H2FMIState;
typedef struct S5L8930NANDRequest S5L8930NANDRequest;
#define S5L8930_H2FMI_BASE 0x81200000u
#define LOG_UNIMP 1
#define LOG_GUEST_ERROR 2
#define HWADDR_PRIx "llx"
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MIN(a,b) ((a)<(b)?(a):(b))
#define ARRAY_SIZE(a) (sizeof(a)/sizeof((a)[0]))
#define le32_to_cpu(x) (x)
#define qemu_log_mask(...) ((void)0)
#define HT(...) ((void)0)
#define g_malloc0(n) calloc(1, (n))
#define g_free free
#define container_of(p, type, member) ((type *)((char *)(p) - offsetof(type, member)))
static int irq_level;
static void qemu_set_irq(int irq, int v) { (void)irq; irq_level = v; }
static int ctz32(uint32_t v) { int n = 0; if (!v) return 32; while (!(v & 1)) { v >>= 1; n++; } return n; }
static int ctpop8(uint8_t v) { int n = 0; while (v) { n += v & 1; v >>= 1; } return n; }
static void stl_le_p(void *p, uint32_t v) {
    uint8_t *b = p; b[0] = v; b[1] = v >> 8; b[2] = v >> 16; b[3] = v >> 24;
}
static uint32_t ldl_le_p(const void *p) {
    const uint8_t *b = p; return b[0] | (uint32_t)b[1] << 8 | (uint32_t)b[2] << 16 | (uint32_t)b[3] << 24;
}
static uint32_t g_page_bytes = 16384;
void s5l8930_iop_nand_info(DeviceState *d, uint32_t *id, uint8_t *m, uint32_t *pb) {
    (void)d; *id = 0x014e5050; *m = 0x01; *pb = g_page_bytes;
}
/* The store: requests wait here until end_request(), in the order started. */
static S5L8930NANDRequest *g_req[16];
static unsigned g_req_n, g_started;
void s5l8930_iop_nand_async(DeviceState *d, S5L8930NANDRequest *req);
void s5l8930_iop_nand_quiesce(DeviceState *d);
void s5l8930_cdma_kick(DeviceState *d) { (void)d; }
void s5l8930_cdma_sink_done(DeviceState *d, uint32_t base, uint32_t size) { (void)d; (void)base; (void)size; }
typedef struct H2FMIBus H2FMIBus;
static void h2fmi_seq_run(H2FMIBus *b);
static void h2fmi_drain(H2FMIBus *b);
static void h2fmi_op_done(S5L8930NANDRequest *req);
static uint64_t h2fmi_read(void *opaque, hwaddr off, unsigned size);
static void h2fmi_write(void *opaque, hwaddr off, uint64_t val, unsigned size);
#include "slice.h"

struct DeviceState { int unused; };
void s5l8930_iop_nand_async(DeviceState *d, S5L8930NANDRequest *req)
{
    (void)d;
    assert(g_req_n < ARRAY_SIZE(g_req));
    g_req[g_req_n++] = req;
    g_started++;
}

/* The oldest request ends: a read fills the page with its row's pattern. */
static void end_request(uint32_t result)
{
    S5L8930NANDRequest *req = g_req[0];

    assert(g_req_n);
    memmove(g_req, g_req + 1, --g_req_n * sizeof(g_req[0]));
    if (req->op == S5L8930_NAND_READ) {
        *req->stride = g_page_bytes + 64;
        memset(req->buf, (uint8_t)req->page, g_page_bytes + 64);
    }
    req->result = result;
    req->done(req);
}

void s5l8930_iop_nand_quiesce(DeviceState *d)
{
    (void)d;
    while (g_req_n) {
        end_request(1);
    }
}

/* Synchronous store calls, which the change removed: the old model made them inside the MMIO handler. */
bool s5l8930_iop_nand_read(DeviceState *d, int bus, uint32_t ce, uint32_t page, uint8_t *buf, uint32_t *stride);
bool s5l8930_iop_nand_read(DeviceState *d, int bus, uint32_t ce, uint32_t page, uint8_t *buf, uint32_t *stride)
{
    (void)d; (void)bus; (void)ce; (void)page; (void)buf; (void)stride;
    fprintf(stderr, "FAIL: a synchronous store read from an MMIO handler\n");
    abort();
}

static void seq(H2FMIBus *b, uint32_t word) { h2fmi_write(b, 0xc0000, word, 4); }

static H2FMIBus *bus(bool ppn)
{
    static DeviceState iop;
    S5L8930H2FMIState *s = calloc(1, sizeof *s);
    H2FMIBus *b = &s->bus[0];

    s->iop = &iop; s->ppn = ppn; s->fmc_off = 0x40000; s->ecc_off = 0x80000;
    for (int i = 0; i < H2FMI_BUSES; i++) {
        s->bus[i].s = s; s->bus[i].n = i;
    }
    return b;
}

int main(void)
{
    /* --- the iPhone 5's FMISS page read --- */
    H2FMIBus *b = bus(true);
    h2fmi_write(b, FMI_FORMAT, 0x20840004, 4);          /* 4 KiB per task, 16 meta bytes */
    h2fmi_write(b, FMC_BASE + FMC_CE, 1, 4);
    h2fmi_write(b, FMC_BASE + 0x20, 7, 4);
    h2fmi_write(b, FMC_BASE + FMC_CMD, 0x370a, 4);
    h2fmi_write(b, FMC_BASE + FMC_ADDR0, 0x4040, 4);
    h2fmi_write(b, FMC_BASE + FMC_ADDR1, 0x100, 4);     /* block 1 page 0: row 256 */
    seq(b, 0x0f040010); seq(b, 0xb);                    /* go: 0x0a, address, 0x37 */
    assert(g_req_n == 1 && g_req[0]->op == S5L8930_NAND_READ && g_req[0]->page == 256);
    seq(b, 0x08000077); seq(b, 0x0800007d);              /* status */
    seq(b, 0x0a004040);                                  /* wait for ready */
    seq(b, 0x14040048);                                  /* the status into the result FIFO */
    seq(b, 0x0800007a);                                  /* DATA_OUT */
    for (int i = 0; i < 4; i++) seq(b, 0x0b000000);
    /* Every write returned with the read in flight: the sequencer waits at its poll, the chip reads busy. */
    assert(g_req_n == 1 && b->seq_code_n == 7 && b->seq_code[0] == 0x0a004040);
    assert(h2fmi_read(b, FMC_BASE + FMC_NAND_STATUS, 4) == 0 && b->seq_result_n == 0 && b->data_len == 0);
    end_request(1);
    assert(b->seq_code_n == 0 && b->seq_result_n == 1 && b->seq_result[0] == 0x40);
    assert(b->data_len == 16384 && b->meta_len == 64 && ldl_le_p(b->data + b->data_off) == 0);
    assert(g_started == 1 && g_req_n == 0);               /* DATA_OUT took the page register's copy */

    /* --- the A4's page read: a status poll and a transfer wait for the chip --- */
    g_page_bytes = 16; g_started = 0;
    b = bus(false);
    h2fmi_write(b, FMC_BASE + FMC_EVTEN, 0x20, 4);
    h2fmi_write(b, FMI_INTEN, FMI_ST_FMC, 4);
    h2fmi_write(b, FMC_BASE + FMC_CE, 1, 4);
    h2fmi_write(b, FMC_BASE + FMC_CMD, 0x3000, 4);
    h2fmi_write(b, FMC_BASE + FMC_ADDR0, 5 << 16, 4);
    h2fmi_write(b, FMC_BASE + FMC_ADDR1, 0, 4);
    h2fmi_write(b, FMC_BASE + FMC_GO, 0xb, 4);
    assert(g_req_n == 1 && g_req[0]->page == 5);
    h2fmi_write(b, FMC_BASE + FMC_EVENTS, ~0u, 4);
    h2fmi_write(b, FMC_BASE + FMC_GO, 0x40, 4);          /* poll status */
    assert(!(h2fmi_read(b, FMC_BASE + FMC_EVENTS, 4) & FMC_EV_STATUS) && !irq_level);
    assert(!(h2fmi_read(b, FMC_BASE + FMC_NAND_STATUS, 4) & 0x40));
    h2fmi_write(b, FMI_CONTROL, 3, 4);                    /* transfer */
    assert(b->data_len == 0 && !(h2fmi_read(b, FMI_STATUS, 4) & FMI_ST_DONE));
    end_request(1);
    assert(h2fmi_read(b, FMC_BASE + FMC_EVENTS, 4) & FMC_EV_STATUS && irq_level);
    assert((h2fmi_read(b, FMC_BASE + FMC_NAND_STATUS, 4) & 0x60) == 0x60);
    assert(b->data_len == 16 && h2fmi_read(b, FMI_STATUS, 4) & FMI_ST_DONE);
    assert(h2fmi_read(b, FMI_DATA, 4) == 0x05050505);

    /* --- a program: busy until it ends, its page register free at once --- */
    h2fmi_write(b, FMI_CONTROL, 0, 4);
    h2fmi_write(b, FMI_STATUS, ~0u, 4);
    h2fmi_write(b, FMC_BASE + FMC_CMD, 0x80, 4);
    h2fmi_write(b, FMC_BASE + FMC_ADDR0, 6 << 16, 4);
    h2fmi_write(b, FMC_BASE + FMC_GO, 0x9, 4);
    h2fmi_write(b, FMI_CONTROL, 5, 4);
    for (unsigned i = 0; i < 16; i += 4) h2fmi_write(b, FMI_DATA, 0xa0a1a2a3, 4);
    for (unsigned i = 0; i < 10; i += 2) h2fmi_write(b, FMI_META, 0xbbbb, 4);
    h2fmi_write(b, FMC_BASE + FMC_CMD, 0x10, 4);
    h2fmi_write(b, FMC_BASE + FMC_GO, 0x1, 4);
    assert(g_req_n == 1 && g_req[0]->op == S5L8930_NAND_PROGRAM && g_req[0]->page == 6 && g_req[0]->len == 16);
    assert(h2fmi_read(b, FMC_BASE + FMC_NAND_STATUS, 4) == NAND_BUSY && b->wpage_len[0] == 0);
    /* The next page fills the register meanwhile; the request keeps its own copy. */
    h2fmi_write(b, FMC_BASE + FMC_CMD, 0x80, 4);
    h2fmi_write(b, FMC_BASE + FMC_GO, 0x9, 4);
    h2fmi_write(b, FMI_CONTROL, 5, 4);
    for (unsigned i = 0; i < 16; i += 4) h2fmi_write(b, FMI_DATA, 0x01020304, 4);
    for (unsigned i = 0; i < 10; i += 2) h2fmi_write(b, FMI_META, 0, 4);
    assert(b->wpage_len[0] == 16 && ldl_le_p(g_req[0]->buf) == 0xa0a1a2a3 && g_req[0]->meta[0] == 0xbb);
    h2fmi_write(b, FMC_BASE + FMC_EVENTS, ~0u, 4);
    h2fmi_write(b, FMC_BASE + FMC_GO, 0x40, 4);
    assert(!(h2fmi_read(b, FMC_BASE + FMC_EVENTS, 4) & FMC_EV_STATUS));
    end_request(0x80000001);                             /* the program failed */
    assert(h2fmi_read(b, FMC_BASE + FMC_EVENTS, 4) & FMC_EV_STATUS);
    assert(h2fmi_read(b, FMC_BASE + FMC_NAND_STATUS, 4) == (NAND_READY | NAND_FAIL));

    /* --- one operation at a time per chip, in order; a reset drops what has not started --- */
    g_started = 0;
    h2fmi_write(b, FMC_BASE + FMC_CMD, 0x3000, 4);
    h2fmi_write(b, FMC_BASE + FMC_ADDR0, 7 << 16, 4);
    h2fmi_write(b, FMC_BASE + FMC_GO, 0xb, 4);
    h2fmi_write(b, FMC_BASE + FMC_ADDR0, 8 << 16, 4);
    h2fmi_write(b, FMC_BASE + FMC_GO, 0xb, 4);
    assert(g_started == 1 && g_req[0]->page == 7 && b->op_head[0]->next);
    end_request(1);
    assert(g_started == 2 && g_req_n == 1 && g_req[0]->page == 8);
    h2fmi_write(b, FMC_BASE + FMC_ADDR0, 9 << 16, 4);
    h2fmi_write(b, FMC_BASE + FMC_GO, 0xb, 4);           /* queued behind page 8 */
    h2fmi_quiesce(b->s, true);
    assert(g_req_n == 0 && g_started == 2 && !b->op_head[0] && !b->op_tail[0] && b->page_ok[0]);

    puts("PASS: H2FMI array operations end off the MMIO path: FMISS read waits at its status poll, A4 poll and "
         "transfer wait for tR, program busy with its register free, per-chip order, reset drops unstarted");
    return 0;
}
