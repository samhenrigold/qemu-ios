/*
 * Host <-> guest pasteboard, shared by the iPod Touch 2G and iPad 1 machines.
 * See include/hw/arm/guest-pasteboard.h.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/error-report.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#include "cpu.h"
#include "hw/arm/guest-pasteboard.h"

/*
 * Host <-> guest pasteboard.
 *
 * The point of this path is that it is not the keyboard. Typing by synthesising
 * taps on iOS's own on-screen keyboard loses exactly the characters people most
 * want to move between the two machines -- punctuation, spaces in URL fields,
 * anything on the symbols page -- because each of those depends on keyboard page
 * state that has no feedback channel. Handing the text to UIPasteboard and
 * letting the user tap Paste has no geometry in it at all.
 *
 * The host cannot write the guest pasteboard by itself: pasteboardd owns the
 * live state and UIPasteboard is its only client (contrib/it-pasteboard/README).
 * So the host only ever parks text here, and the guest agent collects it over
 * the QC_PB_* ops.
 */
static void guest_pb_notify(Notifier *notifier, void *data)
{
    /* We only ever publish; host-side clipboard changes are pushed to the
     * guest explicitly (menu item / qom-set), never automatically. */
}

static void guest_pb_request(QemuClipboardInfo *info,
                                  QemuClipboardType type)
{
    /* Unreachable in practice: we always set the data at the same time as we
     * announce it, and qemu_clipboard_request only calls back for announced
     * types whose data is still missing. */
}


/*
 * How long to give the guest before saying nobody took the text. The agent
 * polls four times a second, so anything it is going to collect it collects
 * almost immediately; this only has to be longer than one poll interval plus
 * the slack of a heavily loaded emulator.
 */
#define PB_WARN_MS 10000

static void guest_pb_warn(void *opaque)
{
    GuestPasteboard *pb = opaque;

    if (!pb->pb_out) {
        return;                 /* collected after all */
    }
    if (pb->pb_polls != pb->pb_polls_at_set) {
        /* Something polled but did not take it. Not the missing-daemon case,
         * so say what it actually is rather than sending anyone to the
         * install instructions. */
        warn_report("pasteboard: the guest agent polled but has not collected "
                    "the text after %d ms", PB_WARN_MS);
        return;
    }

    warn_report("pasteboard: %zu bytes queued for the guest and nothing has "
                "polled for them", pb->pb_out_len);
    if (pb->pb_last_poll_ns == 0) {
        error_printf("         No pasteboard agent has EVER polled this "
                     "machine. it_pbd is almost certainly not installed in "
                     "this NAND image -- setting the property succeeds either "
                     "way, which is why this warning exists.\n"
                     "         Install it: contrib/it-pasteboard/README.md "
                     "(and remember the plist must be owned by root, or "
                     "launchd ignores it without a word).\n"
                     "         Check at any time with:  qom-get "
                     "path=/machine property=pasteboard-agent\n");
    } else {
        error_printf("         The agent last polled %" PRId64 " s ago, so it "
                     "has stopped or died. /var/log/it_pbd.log on the guest "
                     "says which.\n",
                     (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) -
                      pb->pb_last_poll_ns) / NANOSECONDS_PER_SECOND);
    }
}

void guest_pb_set(GuestPasteboard *pb, const char *text)
{
    g_free(pb->pb_out);
    pb->pb_out = NULL;
    pb->pb_out_len = 0;

    if (!text || !*text) {
        if (pb->pb_warn_timer) {
            timer_del(pb->pb_warn_timer);
        }
        return;
    }
    /* A clipboard holds one item. Replacing rather than queueing means a
     * second copy on the host wins, which is what the user just asked for. */
    pb->pb_out = g_strndup(text, QC_PB_MAX_LEN);
    pb->pb_out_len = strlen(pb->pb_out);

    /*
     * Arm the "nobody is listening" check. Handing text to a machine with no
     * guest agent used to be indistinguishable from success from the host
     * side -- no error, no log line, the text just sat in pb_out forever.
     */
    pb->pb_polls_at_set = pb->pb_polls;
    if (!pb->pb_warn_timer) {
        pb->pb_warn_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                          guest_pb_warn, pb);
    }
    timer_mod(pb->pb_warn_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + PB_WARN_MS);
}

static void guest_pb_guest_commit(GuestPasteboard *pb)
{
    QemuClipboardInfo *info;

    g_free(pb->pb_guest);
    pb->pb_guest = pb->pb_in;
    pb->pb_guest_len = pb->pb_in_len;
    pb->pb_in = NULL;
    pb->pb_in_len = 0;

    if (!pb->pb_guest) {
        return;
    }

    /*
     * Registered on first use rather than at machine init: a headless run has
     * no clipboard peer on the other side at all, and the guest text is still
     * readable there through the guest-pasteboard property.
     */
    if (!pb->peer_registered) {
        qemu_clipboard_peer_register(&pb->peer);
        pb->peer_registered = true;
    }

    info = qemu_clipboard_info_new(&pb->peer,
                                   QEMU_CLIPBOARD_SELECTION_CLIPBOARD);
    qemu_clipboard_set_data(&pb->peer, info,
                            QEMU_CLIPBOARD_TYPE_TEXT,
                            pb->pb_guest_len, pb->pb_guest, true);
    qemu_clipboard_info_unref(info);
}

static char *guest_pb_get_pasteboard(GuestPasteboard *pb)
{
        return g_strdup(pb->pb_out ? pb->pb_out : "");
}

static char *guest_pb_get_guest_pasteboard(GuestPasteboard *pb)
{
        return g_strdup(pb->pb_guest ? pb->pb_guest : "");
}

/*
 * "Is anything on the other end?" -- answerable before you rely on it, rather
 * than after wondering why nothing pasted. The agent polls every 250 ms, so a
 * poll inside the last few seconds means it is running right now.
 */
#define PB_ALIVE_NS (5 * NANOSECONDS_PER_SECOND)

static char *guest_pb_get_pb_agent(GuestPasteboard *pb)
{
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    if (pb->pb_last_poll_ns == 0) {
        return g_strdup("absent: nothing has ever polled -- it_pbd is not "
                        "installed or not running (contrib/it-pasteboard)");
    }
    if (now - pb->pb_last_poll_ns > PB_ALIVE_NS) {
        return g_strdup_printf("stale: last polled %" PRId64 " s ago",
                               (now - pb->pb_last_poll_ns) /
                               NANOSECONDS_PER_SECOND);
    }
    return g_strdup_printf("alive: %" PRIu64 " polls", pb->pb_polls);
}

/*
 * "Did the text I sent get there?" -- which is NOT what any of the properties
 * above answer, and the gap cost a whole investigation.
 *
 * "pasteboard" reads back the item still WAITING, so it empties the instant the
 * guest takes it: collected and never-sent are both "". And "guest-pasteboard"
 * is not a readback at all -- it is the last text COPIED INSIDE the guest, and
 * the agent deliberately marks host text as already-seen so it is never echoed
 * back, so host text can never appear there however well the channel works.
 * Watching it for the string you just set is therefore guaranteed to look like
 * a failure. This property is the one that answers the question.
 */
static char *guest_pb_get_pb_status(GuestPasteboard *pb)
{
        int64_t now = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    g_autofree char *agent = guest_pb_get_pb_agent(pb);

    if (pb->pb_out) {
        return g_strdup_printf("queued: %zu bytes still waiting for the guest "
                               "(agent: %s)", pb->pb_out_len, agent);
    }
    if (pb->pb_delivered) {
        /* Truncated: this is a status line, not a transcript. */
        g_autofree char *shown = g_strndup(pb->pb_delivered, 64);
        return g_strdup_printf("delivered: %zu bytes, %" PRId64 " s ago, "
                               "%" PRIu64 " total: \"%s\"%s (agent: %s)",
                               pb->pb_delivered_len,
                               (now - pb->pb_delivered_ns) /
                               NANOSECONDS_PER_SECOND,
                               pb->pb_deliveries, shown,
                               pb->pb_delivered_len > 64 ? "..." : "", agent);
    }
    return g_strdup_printf("idle: nothing has been sent to the guest "
                           "(agent: %s)", agent);
}

static int pb_copy(CPUState *cpu, uint32_t address, uint8_t *data,
                   size_t length, bool write)
{
    if (length && length - 1 > UINT32_MAX - address) {
        return -1;
    }
    return cpu_memory_rw_debug(cpu, address, data, length, write);
}

bool guest_pb_call(GuestPasteboard *pb, CPUState *cpu, qemu_call_t *q,
                   int32_t *err)
{
    uint32_t off = q->args.pb.offset;
    uint32_t len = q->args.pb.length;

    *err = 0;
    switch (q->call_number) {
    case QC_PB_POLL:
        /* The only proof the host ever gets that a guest agent exists. */
        pb->pb_polls++;
        pb->pb_last_poll_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        q->retval = pb->pb_out ? (int64_t)pb->pb_out_len : 0;
        return true;
    case QC_PB_READ:
        if (!pb->pb_out || off >= pb->pb_out_len) {
            q->retval = 0;
            return true;
        }
        if (len > pb->pb_out_len - off) {
            len = pb->pb_out_len - off;
        }
        if (pb_copy(cpu, q->args.pb.buffer_guest_ptr,
                    (uint8_t *)pb->pb_out + off, len, true)) {
            q->retval = -1;
            *err = EFAULT;
            return true;
        }
        q->retval = len;
        return true;
    case QC_PB_ACK:
        /*
         * Keep what was taken rather than dropping it: this is the only moment
         * the host learns that a queued item reached the guest. See
         * pb_delivered.
         */
        if (pb->pb_out) {
            g_free(pb->pb_delivered);
            pb->pb_delivered = pb->pb_out;
            pb->pb_delivered_len = pb->pb_out_len;
            pb->pb_delivered_ns = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
            pb->pb_deliveries++;
        }
        pb->pb_out = NULL;
        pb->pb_out_len = 0;
        q->retval = 0;
        return true;
    case QC_PB_WRITE:
        /*
         * offset 0 starts a fresh item. Anything else has to continue the one
         * already being staged; a gap would leave uninitialised bytes in the
         * middle of the text.
         */
        if (off == 0) {
            g_clear_pointer(&pb->pb_in, g_free);
            pb->pb_in_len = 0;
        }
        if (off != pb->pb_in_len || len > QC_PB_MAX_LEN ||
            off + (size_t)len > QC_PB_MAX_LEN) {
            g_clear_pointer(&pb->pb_in, g_free);
            pb->pb_in_len = 0;
            *err = EINVAL;
            q->retval = -1;
            return true;
        }
        pb->pb_in = g_realloc(pb->pb_in, off + len + 1);
        if (pb_copy(cpu, q->args.pb.buffer_guest_ptr,
                    (uint8_t *)pb->pb_in + off, len, false)) {
            g_clear_pointer(&pb->pb_in, g_free);
            pb->pb_in_len = 0;
            q->retval = -1;
            *err = EFAULT;
            return true;
        }
        pb->pb_in_len = off + len;
        pb->pb_in[pb->pb_in_len] = '\0';
        q->retval = len;
        return true;
    case QC_PB_COMMIT:
        if (!pb->pb_in) {
            q->retval = -1;
            *err = EINVAL;
            return true;
        }
        guest_pb_guest_commit(pb);
        q->retval = 0;
        return true;
    default:
        return false;
    }
}

/* QOM string properties whose opaque is the GuestPasteboard. */
typedef char *(*PbGetter)(GuestPasteboard *pb);

typedef struct PbProp {
    GuestPasteboard *pb;
    PbGetter get;
} PbProp;

static void pb_prop_get(Object *obj, Visitor *v, const char *name,
                        void *opaque, Error **errp)
{
    PbProp *p = opaque;
    g_autofree char *value = p->get(p->pb);
    visit_type_str(v, name, &value, errp);
}

static void pb_prop_set(Object *obj, Visitor *v, const char *name,
                        void *opaque, Error **errp)
{
    PbProp *p = opaque;
    g_autofree char *value = NULL;
    if (visit_type_str(v, name, &value, errp)) {
        guest_pb_set(p->pb, value);
    }
}

static void pb_prop_release(Object *obj, const char *name, void *opaque)
{
    g_free(opaque);
}

static void pb_prop_add(Object *obj, GuestPasteboard *pb, const char *name,
                        PbGetter get, bool settable, const char *desc)
{
    PbProp *p = g_new(PbProp, 1);
    p->pb = pb;
    p->get = get;
    object_property_add(obj, name, "string", pb_prop_get,
                        settable ? pb_prop_set : NULL, pb_prop_release, p);
    object_property_set_description(obj, name, desc);
}

void guest_pb_init(GuestPasteboard *pb, Object *machine, const char *peer_name)
{
    pb->peer.name = peer_name;
    pb->peer.notifier.notify = guest_pb_notify;
    pb->peer.request = guest_pb_request;

    /*
     * Setting "pasteboard" from QMP is the headless equivalent of the Cocoa
     * "Paste Text to Guest" menu item:
     *
     *   qom-set  path=/machine property=pasteboard value="Hello. World #1"
     *   qom-get  path=/machine property=pasteboard-status
     *
     * pasteboard-status, NOT guest-pasteboard, is how you check that the text
     * arrived. guest-pasteboard is the other direction, and the agent
     * suppresses the echo of anything the host sent, so polling it for the
     * string you just set is a guaranteed false negative.
     */
    pb_prop_add(machine, pb, "pasteboard", guest_pb_get_pasteboard, true,
        "Text to hand to the guest's UIPasteboard. Collected by the guest "
        "pasteboard agent (contrib/it-pasteboard/it_pbd.c), after which the "
        "user pastes it wherever they like -- unlike the on-screen-keyboard "
        "typist, punctuation and symbols survive. Reads back only what is "
        "still WAITING to be collected, so it empties as soon as the guest "
        "takes it -- read pasteboard-status to see whether it arrived");
    pb_prop_add(machine, pb, "guest-pasteboard", guest_pb_get_guest_pasteboard,
        false,
        "GUEST -> HOST only: the last text copied inside the guest, as "
        "reported by the pasteboard agent. NOT a readback of what the host "
        "sent -- the agent suppresses that echo deliberately, so text set "
        "through the 'pasteboard' property never appears here. Also pushed to "
        "the host clipboard when a UI with a clipboard peer is attached");
    pb_prop_add(machine, pb, "pasteboard-agent", guest_pb_get_pb_agent, false,
        "Whether a guest pasteboard agent is actually running: 'alive', "
        "'stale' or 'absent'. Setting the pasteboard succeeds whether or not "
        "anything is listening, so ask this before believing it");
    pb_prop_add(machine, pb, "pasteboard-status", guest_pb_get_pb_status, false,
        "Whether host -> guest text actually reached the guest agent: "
        "'queued' (still waiting), 'delivered' (with the text, its size and "
        "how long ago) or 'idle'. This is the readback the other three "
        "properties cannot give you");
}
