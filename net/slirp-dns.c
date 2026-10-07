/*
 * macOS: answer the guest's DNS through the system resolver (mDNSResponder).
 *
 * With lan=off, slirp would otherwise send the guest's queries as raw UDP to
 * the Mac's DNS server -- usually the LAN router -- and that one datagram is a
 * Local Network access: macOS asks the user for permission although the guest
 * is kept off the LAN. A resolver on 127.0.0.1 (slirp_set_dns_forward) takes
 * the queries instead and asks DNSServiceQueryRecord, which is exempt. Names
 * only multicast DNS answers (.local, link-local reverse zones) are refused:
 * resolving them is itself a local-network operation.
 *
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 */

#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "qemu/bswap.h"
#include "qemu/thread.h"
#include "net/slirp.h"
#include "qemu/sockets.h"
#include <dns_sd.h>
#include <poll.h>

#define DNS_MAX 512                 /* a UDP answer without EDNS */
#define DNS_TIMEOUT_MS 5000
#define DNS_MAX_PENDING 64

static int dns_fd = -1;
static int dns_pending;

typedef struct DNSJob {
    struct sockaddr_in from;
    uint8_t buf[DNS_MAX];
    size_t len;         /* the reply so far: header, question, answers */
    uint16_t qtype, answers;
    int rcode;
    bool done, truncated;
} DNSJob;

static bool mdns_only(const char *name)
{
    static const char *const zones[] = {
        ".local", ".254.169.in-addr.arpa", ".8.e.f.ip6.arpa", ".9.e.f.ip6.arpa",
        ".a.e.f.ip6.arpa", ".b.e.f.ip6.arpa",
    };
    size_t n = strlen(name);

    for (size_t i = 0; i < ARRAY_SIZE(zones); i++) {
        size_t z = strlen(zones[i]);
        if (!g_ascii_strcasecmp(name, zones[i] + 1) ||
            (n > z && !g_ascii_strcasecmp(name + n - z, zones[i]))) {
            return true;
        }
    }
    return false;
}

static void DNSSD_API dns_record(DNSServiceRef ref, DNSServiceFlags flags,
                                 uint32_t ifindex, DNSServiceErrorType err,
                                 const char *fullname, uint16_t rrtype,
                                 uint16_t rrclass, uint16_t rdlen,
                                 const void *rdata, uint32_t ttl, void *opaque)
{
    DNSJob *j = opaque;

    if (err == kDNSServiceErr_NoSuchName) {
        j->rcode = 3;                                   /* NXDOMAIN */
    } else if (err != kDNSServiceErr_NoError &&
               err != kDNSServiceErr_NoSuchRecord) {
        j->rcode = 2;                                   /* SERVFAIL */
    } else if (err == kDNSServiceErr_NoError && (flags & kDNSServiceFlagsAdd) &&
               (rrtype == j->qtype || j->qtype == 255)) {
        /* Every answer is for the question's name (a pointer to it): a CNAME
         * chain the system resolver followed reads as the final records. */
        if (j->len + 12 + rdlen > DNS_MAX) {
            j->truncated = true;
        } else {
            uint8_t *p = j->buf + j->len;
            p[0] = 0xc0; p[1] = 12;
            stw_be_p(p + 2, rrtype);
            stw_be_p(p + 4, rrclass);
            stl_be_p(p + 6, ttl);
            stw_be_p(p + 10, rdlen);
            memcpy(p + 12, rdata, rdlen);
            j->len += 12 + rdlen;
            j->answers++;
        }
    } else if (err == kDNSServiceErr_NoError) {
        return;     /* a removal, or an intermediate CNAME: more to come */
    }
    if (!(flags & kDNSServiceFlagsMoreComing)) {
        j->done = true;
    }
}

static void *dns_answer(void *opaque)
{
    DNSJob *j = opaque;
    uint8_t *b = j->buf;
    char name[256];
    size_t at = 12, n = 0;
    uint16_t qclass = 0;

    j->rcode = 1;                                       /* FORMERR */
    if ((b[2] & 0x78) != 0) {
        j->rcode = 4;                                   /* NOTIMP: not QUERY */
        at = 0;
    } else if (lduw_be_p(b + 4) == 1) {
        while (at < j->len && b[at] && b[at] < 64 && at + 1 + b[at] < j->len &&
               n + b[at] + 1 < sizeof(name)) {
            memcpy(name + n, b + at + 1, b[at]);
            n += b[at];
            name[n++] = '.';
            at += 1 + b[at];
        }
        if (at + 5 <= j->len && !b[at]) {
            name[n ? n - 1 : 0] = '\0';
            j->qtype = lduw_be_p(b + at + 1);
            qclass = lduw_be_p(b + at + 3);
            at += 5;
            j->rcode = 2;                               /* SERVFAIL until answered */
        } else {
            at = 0;
        }
    } else {
        at = 0;
    }

    j->len = at ? at : 12;
    if (at && mdns_only(name)) {
        j->rcode = 3;
    } else if (at) {
        DNSServiceRef ref;
        if (DNSServiceQueryRecord(&ref, kDNSServiceFlagsReturnIntermediates,
                                  kDNSServiceInterfaceIndexAny, name, j->qtype,
                                  qclass, dns_record, j) == kDNSServiceErr_NoError) {
            struct pollfd pfd = { .fd = DNSServiceRefSockFD(ref), .events = POLLIN };
            int64_t deadline = g_get_monotonic_time() + DNS_TIMEOUT_MS * 1000;
            j->rcode = 0;
            while (!j->done) {
                int64_t left = (deadline - g_get_monotonic_time()) / 1000;
                if (left <= 0 || poll(&pfd, 1, left) <= 0 ||
                    DNSServiceProcessResult(ref) != kDNSServiceErr_NoError) {
                    if (!j->answers) {
                        j->rcode = 2;
                    }
                    break;
                }
            }
            DNSServiceRefDeallocate(ref);
        }
    }

    b[2] = 0x80 | (b[2] & 0x79) | (j->truncated ? 0x02 : 0);   /* QR, RD */
    b[3] = 0x80 | j->rcode;                                     /* RA */
    stw_be_p(b + 4, at ? 1 : 0);
    stw_be_p(b + 6, j->answers);
    stw_be_p(b + 8, 0);
    stw_be_p(b + 10, 0);
    sendto(dns_fd, b, j->len, 0, (struct sockaddr *)&j->from, sizeof(j->from));
    g_free(j);
    qatomic_dec(&dns_pending);
    return NULL;
}

static void *dns_serve(void *opaque)
{
    for (;;) {
        DNSJob *j = g_new0(DNSJob, 1);
        socklen_t fl = sizeof(j->from);
        ssize_t r = recvfrom(dns_fd, j->buf, sizeof(j->buf), 0,
                             (struct sockaddr *)&j->from, &fl);
        if (r < 12 || qatomic_fetch_inc(&dns_pending) >= DNS_MAX_PENDING) {
            if (r >= 12) {
                qatomic_dec(&dns_pending);
            }
            g_free(j);
            continue;
        }
        j->len = r;
        QemuThread t;
        qemu_thread_create(&t, "slirp-dns", dns_answer, j, QEMU_THREAD_DETACHED);
    }
    return NULL;
}

bool net_slirp_dns_forward(struct sockaddr_in *addr)
{
    static struct sockaddr_in bound;

    if (dns_fd < 0) {
        int fd = qemu_socket(AF_INET, SOCK_DGRAM, 0);
        socklen_t l = sizeof(bound);
        bound = (struct sockaddr_in) {
            .sin_len = sizeof(bound), .sin_family = AF_INET,
            .sin_addr.s_addr = htonl(INADDR_LOOPBACK),
        };
        if (fd < 0 || bind(fd, (struct sockaddr *)&bound, sizeof(bound)) ||
            getsockname(fd, (struct sockaddr *)&bound, &l)) {
            if (fd >= 0) {
                close(fd);
            }
            return false;
        }
        dns_fd = fd;
        QemuThread t;
        qemu_thread_create(&t, "slirp-dns", dns_serve, NULL, QEMU_THREAD_DETACHED);
    }
    *addr = bound;
    return true;
}
