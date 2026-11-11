/* A native Synopsys periodic skeleton QH must not advance into qTD zero.
 * Exercise production fetch/dispatch, retaining real async and periodic work.
 * SLICE:types hw/usb/hcd-ehci.h define QTD_TOKEN_
 * SLICE:types hw/usb/hcd-ehci.c define NLPTR_
 * SLICE:types hw/usb/hcd-ehci.h typedef EHCIqh
 * SLICE hw/usb/hcd-ehci.c fn ehci_state_fetchqh
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "types.h"

#define get_field(v, f) (((v) & f##_MASK) >> f##_SH)
#define EHCI_DEBUG 0
#define DPRINTF(...) ((void)0)
#define USBSTS_REC 0x2000
enum { EST_ACTIVE, EST_HORIZONTALQH, EST_FETCHQTD, EST_ADVANCEQUEUE };
typedef struct EHCIState EHCIState;
typedef struct EHCIQueue {
    unsigned seen, transact_ctr;
    uint32_t qhaddr, qtdaddr;
    int async;
    EHCIState *ehci;
    EHCIqh qh;
    void *dev;
} EHCIQueue;
struct EHCIState {
    uint32_t usbsts;
    int state;
    bool skip_idle_periodic_qh;
    EHCIQueue queue;
    EHCIqh memory;
};
static uint32_t ehci_get_fetch_addr(EHCIState *s, int a)
{
    return s->queue.qhaddr;
}
static EHCIQueue *ehci_find_queue_by_qh(EHCIState *s, uint32_t e, int a)
{
    return &s->queue;
}
static EHCIQueue *ehci_alloc_queue(EHCIState *s, uint32_t e, int a)
{
    assert(0);
    return 0;
}
static void ehci_set_state(EHCIState *s, int a, int state)
{
    s->state = state;
}
static int get_dwords(EHCIState *s, uint32_t a, uint32_t *p, int n)
{
    assert(a == 0x82c06120 && n == sizeof(EHCIqh) / 4);
    memcpy(p, &s->memory, sizeof(EHCIqh));
    return n;
}
#define ehci_trace_qh(...) ((void)0)
#define ehci_trace_guest_bug(...) ((void)0)
static bool ehci_verify_qh(EHCIQueue *q, EHCIqh *h)
{
    return true;
}
static int ehci_reset_queue(EHCIQueue *q)
{
    assert(0);
    return 0;
}
static void *ehci_find_device(EHCIState *s, uint32_t a)
{
    return s;
}
static void ehci_clear_usbsts(EHCIState *s, uint32_t bits)
{
    s->usbsts &= ~bits;
}
#include "slice.h"
static bool skip = true;
static int dispatch(int async, uint32_t epchar, uint32_t mask, uint32_t token,
                    uint32_t current)
{
    EHCIState s = {.skip_idle_periodic_qh = skip, .memory = {.next = 1,
                              .epchar = epchar,
                              .epcap = 0x40000000 | mask,
                              .next_qtd = 0,
                              .altnext_qtd = 1,
                              .token = token,
                              .current_qtd = current}};
    s.queue = (EHCIQueue){.ehci = &s, .qhaddr = 0x82c06120, .async = async};
    assert(ehci_state_fetchqh(&s, async) == &s.queue);
    return s.state;
}
int main(void)
{
    /* Actual idle anchor captured from the native 12A365 driver. */
    assert(dispatch(0, 0x00082f00, 0, 0, 0) == EST_HORIZONTALQH);
    assert(dispatch(0, 0x00082f00, 0, QTD_TOKEN_ACTIVE, 0x83000000) ==
           EST_HORIZONTALQH);
    /* Zero service masks remain valid for asynchronous control traffic. */
    assert(dispatch(1, 0x00406000, 0, 0, 0) == EST_ADVANCEQUEUE);
    assert(dispatch(0, 0x00082f00, 1, 0, 0) == EST_ADVANCEQUEUE);
    assert(dispatch(0, 0x00082f00, 0x80, QTD_TOKEN_ACTIVE, 0x83000000) ==
           EST_FETCHQTD);
    /* Split endpoint state and ordinary halt behavior remain unchanged. */
    assert(dispatch(0, 0x00080f00, 0, 0, 0) == EST_ADVANCEQUEUE);
    assert(dispatch(1, 0x00406000, 0, QTD_TOKEN_HALT, 0) == EST_HORIZONTALQH);
    /* Off (every board but the A6), the anchor is fetched as it always was. */
    skip = false;
    assert(dispatch(0, 0x00082f00, 0, 0, 0) == EST_ADVANCEQUEUE);
    puts(
        "PASS: native zero-mask periodic anchors skip qTD fetch; real periodic/async work preserved");
}
