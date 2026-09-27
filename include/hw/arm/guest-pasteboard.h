/*
 * Host <-> guest pasteboard, shared by the iPod Touch 2G and iPad 1 machines.
 * The guest half is contrib/it-pasteboard/it_pbd.c (and it_agent on the iPod),
 * speaking the QC_PB_* ops of include/hw/arm/guest-services/general.h over the
 * cp15 QEMU_CALL channel.
 */
#ifndef HW_ARM_GUEST_PASTEBOARD_H
#define HW_ARM_GUEST_PASTEBOARD_H

#include "qom/object.h"
#include "qemu/timer.h"
#include "ui/clipboard.h"
#include "cpu.h"                     /* general.h needs CPUARMState */
#include "hw/arm/guest-services/general.h"

typedef struct GuestPasteboard {
    /*
     * pb_out is what the host has queued for the guest; the guest agent polls
     * for it, reads it out in chunks and acknowledges. pb_in is the staging
     * buffer the guest fills going the other way, published to the host
     * clipboard on commit and kept in pb_guest so it can be read back.
     */
    char *pb_out;             /* text waiting for the guest, or NULL */
    size_t pb_out_len;
    char *pb_in;              /* partial text arriving from the guest */
    size_t pb_in_len;
    char *pb_guest;           /* last text the guest published, or NULL */
    size_t pb_guest_len;
    QemuClipboardPeer peer;
    bool peer_registered;

    /*
     * Liveness. Without this, setting the pasteboard on an image that has no
     * guest agent in it looks exactly like success: the property takes the
     * text, nothing complains, and the text simply sits here forever. That is
     * not hypothetical -- it is how the whole feature was reported working
     * while being dead on every image the runner actually boots. So record
     * when the guest last polled, warn if a queued item is never collected,
     * and expose the answer through the "pasteboard-agent" property.
     */
    int64_t pb_last_poll_ns;  /* 0 = the guest has never polled */
    uint64_t pb_polls;
    uint64_t pb_polls_at_set; /* pb_polls when the pending item was queued */
    QEMUTimer *pb_warn_timer;

    /*
     * Delivery. Liveness above answers "is an agent there"; this answers "did
     * my text reach it", which is a different question and the one people
     * actually ask. pb_out is cleared on ACK, so a delivered item and an item
     * never queued would read identically ("" from the "pasteboard" property),
     * and "guest-pasteboard" cannot stand in for a readback -- the agent
     * deliberately records host text as already-seen so it is never echoed
     * back. Kept here and reported through "pasteboard-status".
     */
    char *pb_delivered;       /* last text the guest agent collected, or NULL */
    size_t pb_delivered_len;
    int64_t pb_delivered_ns;
    uint64_t pb_deliveries;
} GuestPasteboard;

/*
 * Add the pasteboard, guest-pasteboard, pasteboard-agent and pasteboard-status
 * properties to the machine object; peer_name names the host clipboard peer.
 */
void guest_pb_init(GuestPasteboard *pb, Object *machine, const char *peer_name);

/*
 * Queue text for the guest's UIPasteboard. Replaces anything not yet collected
 * -- a clipboard has one item, and a stale one is worse than none. Safe to call
 * with the guest agent absent; the text simply sits there.
 */
void guest_pb_set(GuestPasteboard *pb, const char *text);

/*
 * Serve one QC_PB_* request in place (retval and *err). Returns false, leaving
 * q untouched, for any other call number.
 */
bool guest_pb_call(GuestPasteboard *pb, CPUState *cpu, qemu_call_t *q,
                   int32_t *err);

#endif
