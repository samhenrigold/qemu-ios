/*
 * it_agent -- the guest half of the host<->guest clipboard.
 *
 * A launchd-started daemon that carries text between the host's clipboard and
 * the guest's UIPasteboard, over the cp15 QEMU_CALL tunnel (QC_PB_* in
 * include/hw/arm/guest-services/general.h).
 *
 * Why a guest process at all: pasteboardd owns the live pasteboard state and
 * UIPasteboard is its only client, so no amount of host-side file editing can
 * write it. See README.md.
 *
 * Why a plain C daemon rather than a dylib injected into SpringBoard: nothing
 * here needs to be in any particular process. The pasteboard is system-wide,
 * and a bundle-less root binary sets it just fine (that is what pbset proved).
 * A separate process also cannot wedge SpringBoard's launch, which is the trap
 * contrib/it-kbd-agent had to be built around.
 *
 * it_pbd (contrib/it-pasteboard) is this clipboard alone: it includes this file with
 * IT_AGENT_CLIPBOARD_ONLY and brings its own main.
 *
 * THE TRAP, repeated here because it costs a reboot to rediscover: never call
 * -[UIPasteboard setString:], -string or -pasteboardTypes from a process like
 * this. They send an empty type string, pasteboardd turns it into a NULL
 * CFDictionary key, and the daemon dies of SIGBUS -- after which every later
 * pasteboard call in the system silently returns nil, which looks exactly like
 * an empty pasteboard. Every call below passes public.utf8-plain-text
 * explicitly.
 *
 * Build: ./build.sh (see ../armv6-toolchain/README.md). Plain C with a dlopen'd
 * ObjC runtime, because `ld -lobjc` against the 3.1.3 SDK is fatal.
 */

#include <unistd.h>
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);

#ifndef RTLD_NOW
#define RTLD_NOW 2
#endif

typedef unsigned int uint32_t;
typedef long long int64_t;

#define QC_PB_POLL   0x150
#define QC_PB_READ   0x151
#define QC_PB_ACK    0x152
#define QC_PB_WRITE  0x153
#define QC_PB_COMMIT 0x154

#include "../armv6-toolchain/qemu-call.h"

static unsigned long long agent_token;

static int64_t qc(uint32_t call, void *buf, uint32_t off, uint32_t len)
{
    qemu_call_t q;
    unsigned char *p = (unsigned char *)&q;
    unsigned i;
    for (i = 0; i < sizeof(q); i++) {
        p[i] = 0;
    }
    q.call_number = call;
    if (call >= 0x160) q.ag.token = agent_token;
    q.ag.buffer = (uint32_t)buf;
    q.ag.offset = off;
    q.ag.length = len;
    qemu_call_trap(&q);
    return q.retval;
}

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void w(const char *s) { write(2, s, slen(s)); }

/* ObjC, resolved at run time. */
typedef void *id_;
typedef void *SEL_;
static id_ (*p_getClass)(const char *);
static SEL_ (*p_sel)(const char *);
static id_ (*m0)(id_, SEL_);
static id_ (*m1s)(id_, SEL_, const char *);
static id_ (*m1)(id_, SEL_, id_);
static id_ (*m2)(id_, SEL_, id_, id_);
static long (*mlong)(id_, SEL_);
#define S(n) p_sel(n)
#define C(n) p_getClass(n)

static id_ s_uti;          /* @"public.utf8-plain-text", retained forever */
static SEL_ sel_general, sel_setValue, sel_dataForType, sel_bytes, sel_length;
static SEL_ sel_alloc, sel_init, sel_release, sel_retain, sel_stringWithUTF8;
static id_ cls_pool, cls_string, cls_pasteboard;

/*
 * Both directions share these. QC_PB_MAX_LEN on the host is 256 KiB, but the
 * guest has no reason to move that much text by hand and a fixed buffer keeps
 * this daemon allocation-free. Text longer than PB_BUF - 1 bytes is not moved
 * at all, either way: a clipboard cut short mid-text (or mid-character) pastes
 * something nobody copied. The host's side holds the same cap.
 */
#define PB_BUF 65536
#define PB_CHUNK 1024
static char pb_in[PB_BUF];    /* text arriving from the host */
static char pb_last[PB_BUF];  /* the guest text we last told the host about */
/* -1 = nothing observed yet: the first look is the pasteboard as the guest
 * kept it (pasteboardd persists it across reboots), not a copy, so it is
 * recorded and not published -- else every boot would put stale guest text
 * over whatever the host's clipboard holds. -2 = text over the cap. */
static long pb_last_len = -1;

static int objc_setup(void)
{
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW);
    dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", RTLD_NOW);
    void *uikit = dlopen("/System/Library/Frameworks/UIKit.framework/UIKit", RTLD_NOW);
    if (!objc || !uikit) {
        return 0;
    }
    p_getClass = (id_ (*)(const char *))dlsym(objc, "objc_getClass");
    p_sel = (SEL_ (*)(const char *))dlsym(objc, "sel_registerName");
    void *msg = dlsym(objc, "objc_msgSend");
    if (!p_getClass || !p_sel || !msg) {
        return 0;
    }
    m0 = (id_ (*)(id_, SEL_))msg;
    m1s = (id_ (*)(id_, SEL_, const char *))msg;
    m1 = (id_ (*)(id_, SEL_, id_))msg;
    m2 = (id_ (*)(id_, SEL_, id_, id_))msg;
    mlong = (long (*)(id_, SEL_))msg;

    cls_pool = C("NSAutoreleasePool");
    cls_string = C("NSString");
    cls_pasteboard = C("UIPasteboard");
    sel_alloc = S("alloc");
    sel_init = S("init");
    sel_release = S("release");
    sel_retain = S("retain");
    sel_stringWithUTF8 = S("stringWithUTF8String:");
    sel_general = S("generalPasteboard");
    sel_setValue = S("setValue:forPasteboardType:");
    sel_dataForType = S("dataForPasteboardType:");
    sel_bytes = S("bytes");
    sel_length = S("length");
    if (!cls_pool || !cls_string || !cls_pasteboard) {
        return 0;
    }
    /* Retained so it outlives every pool below and is never handed to a
     * released string -- and built inside a pool of its own, because there is
     * no ambient one in main() and Foundation complains (and leaks) without. */
    id_ pool = m0(m0(cls_pool, sel_alloc), sel_init);
    s_uti = m0(m1s(cls_string, sel_stringWithUTF8, "public.utf8-plain-text"),
               sel_retain);
    m0(pool, sel_release);
    return s_uti != 0;
}

/* Host -> guest. Returns 1 if something was pasted in. */
static int pump_host_to_guest(void)
{
    int64_t n = qc(QC_PB_POLL, 0, 0, 0);
    if (n <= 0) {
        return 0;
    }
    if (n > PB_BUF - 1) {
        qc(QC_PB_ACK, 0, 0, 0);   /* over the cap: retire it unread */
        return 0;
    }

    long off = 0;
    while (off < n) {
        uint32_t want = (uint32_t)(n - off);
        if (want > PB_CHUNK) {
            want = PB_CHUNK;
        }
        int64_t got = qc(QC_PB_READ, pb_in + off, (uint32_t)off, want);
        if (got <= 0) {
            break;
        }
        off += (long)got;
    }
    pb_in[off] = 0;
    /* Retire it either way: a text we could not read is not going to become
     * readable, and leaving it pending would spin this loop forever. */
    qc(QC_PB_ACK, 0, 0, 0);
    if (off <= 0) {
        return 0;
    }

    id_ pool = m0(m0(cls_pool, sel_alloc), sel_init);
    id_ pb = m0(cls_pasteboard, sel_general);
    if (pb) {
        id_ s = m1s(cls_string, sel_stringWithUTF8, pb_in);
        if (s) {
            m2(pb, sel_setValue, s, s_uti);
        }
    }
    m0(pool, sel_release);

    /*
     * Remember it as already-seen, so the guest->host direction does not
     * immediately echo the host's own text back at it.
     */
    long i;
    for (i = 0; i < off; i++) {
        pb_last[i] = pb_in[i];
    }
    pb_last_len = off;
    return 1;
}

/* Guest -> host. Publishes when the guest pasteboard's text changes. */
static void pump_guest_to_host(void)
{
    id_ pool = m0(m0(cls_pool, sel_alloc), sel_init);
    id_ pb = m0(cls_pasteboard, sel_general);
    const char *bytes = 0;
    long len = 0;

    if (pb) {
        /* Explicit UTI. dataForPasteboardType: with a real type is the safe
         * read; -string and -pasteboardTypes are the ones that kill the
         * daemon. */
        id_ d = m1(pb, sel_dataForType, s_uti);
        if (d) {
            len = mlong(d, sel_length);
            bytes = (const char *)m0(d, sel_bytes);
        }
    }
    if (!bytes || len < 0) {
        len = 0;
    }
    if (len > PB_BUF - 1) {
        pb_last_len = -2;
        m0(pool, sel_release);
        return;
    }

    int baseline = (pb_last_len == -1);
    int same = (len == pb_last_len);
    long i;
    if (same) {
        for (i = 0; i < len; i++) {
            if (pb_last[i] != bytes[i]) {
                same = 0;
                break;
            }
        }
    }
    if (!same) {
        for (i = 0; i < len; i++) {
            pb_last[i] = bytes[i];
        }
        pb_last_len = len;
        if (len > 0 && !baseline) {
            long off = 0;
            while (off < len) {
                uint32_t put = (uint32_t)(len - off);
                if (put > PB_CHUNK) {
                    put = PB_CHUNK;
                }
                if (qc(QC_PB_WRITE, pb_last + off, (uint32_t)off, put) < 0) {
                    off = -1;
                    break;
                }
                off += put;
            }
            if (off == len) {
                qc(QC_PB_COMMIT, 0, 0, 0);
                pb_last[len] = 0;
            }
        }
    }
    m0(pool, sel_release);
}

/*
 * Seconds to wait before the first clipboard tick. Nothing here runs inside SpringBoard,
 * so this is not the launch hazard the keyboard agent had -- but pasteboardd is
 * launched on demand and there is no point waking it before there is a UI to
 * paste into.
 */
#define STARTUP_DELAY 40
#define CLIPBOARD_UNAVAILABLE 2     /* the worker's exit status: no pasteboard here (2.x), never respawned */

static int clipboard_main(void)
{
    sleep(STARTUP_DELAY);
    if (!objc_setup()) {
        w("it_agent: clipboard runtime unavailable\n");
        return CLIPBOARD_UNAVAILABLE;
    }
    for (;;) {
        if (!pump_host_to_guest()) pump_guest_to_host();
        usleep(250000);
    }
}

#ifndef IT_AGENT_CLIPBOARD_ONLY   /* contrib/it-pasteboard/it_pbd.c: the clipboard above, its own main */
#include "agent-ops.h"

int main(int argc, char **argv, char **environment);
/* Without LEGACY_LINK, armv6.sh's link has no crt1: the process enters at its entry point with argc, argv[], 0,
 * envp[] on the stack only, and main's registers hold garbage (an armv7 agent entered at _main died on argv[0]
 * every boot). The build scripts enter here (-e __start) instead; LEGACY_LINK's crt1old.c does the same. */
__attribute__((naked, used)) void _start(void)
{
    __asm__ volatile("ldr r0, [sp]\n\t"
                     "add r1, sp, #4\n\t"
                     "add r2, r0, #1\n\t"
                     "add r2, r1, r2, lsl #2\n\t"
                     "bic sp, sp, #7\n\t"
                     "bl _main\n\t"
                     "bl __exit");
}

int main(int argc, char **argv, char **environment)
{
    if (argc == 2 && !strcmp(argv[1], "--clipboard-worker")) {
        return clipboard_main();
    }
    pid_t clipboard = 0;
    unsigned clipboard_retry = 0;
    int clipboard_status, clipboard_off = 0;
    unsigned ticks = 0;
    w("it_agent: command service up\n");
    for (;;) {
        /* File/command RPC needs no UIKit initialization. Start immediately so
         * boot-time provisioning does not wait for the clipboard grace period. */
        agent_tick();
        /* UIPasteboard makes synchronous IPC calls. A sleeping or unavailable
         * pasteboard server must not stop command polling or child reaping.
         * Re-exec a worker rather than forking after ObjC has initialized. It
         * inherits this launchd job's process group for normal job cleanup. */
        if (clipboard && waitpid(clipboard, &clipboard_status, WNOHANG) == clipboard) {
            clipboard = 0;
            clipboard_retry = 20;
            /* as main(void) did: a runtime without the pasteboard is tried once, not every 45 s for good */
            clipboard_off = WIFEXITED(clipboard_status) && WEXITSTATUS(clipboard_status) == CLIPBOARD_UNAVAILABLE;
        }
        if (!clipboard && !clipboard_retry && !clipboard_off && argv[0] && argv[0][0] == '/') {
            char *args[] = {argv[0], "--clipboard-worker", 0};
            if (posix_spawn(&clipboard, argv[0], 0, 0, args, environment)) {
                clipboard = 0;
                clipboard_retry = 20;
            }
        }
        if (clipboard_retry) clipboard_retry--;
        /* The app's Stop is a hard halt (pause, flush the host's files, quit), never a guest
         * shutdown, and the kernel's own writeback is ~30 s: flush every 5 s so what was
         * written 5+ s before a Stop survives it. ponytail: fixed period; nothing to do when
         * clean, make it adaptive only if flash wear or pauses ever show up. */
        if (++ticks % 20 == 0) sync();
        usleep(250000);
    }
    return 0;
}
#endif
