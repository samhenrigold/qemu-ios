#!/bin/bash
# With egress restricted, a guest's DNS query to the virtual nameserver gets an immediate NXDOMAIN with a zero-TTL root
# SOA, and anything else stays dropped (udp_refuse_dns, from subprojects/packagefiles/libslirp-restricted-dns.patch, so
# no libslirp checkout is needed).
set -eu
HERE="$(cd "$(dirname "$0")" && pwd)"
PATCH="$HERE/../../subprojects/packagefiles/libslirp-restricted-dns.patch"
WRAP="$HERE/../../subprojects/slirp.wrap"
grep -q 'libslirp-restricted-dns.patch' "$WRAP" || { echo "FAIL: the wrap doesn't apply the patch"; exit 1; }
grep -q '^+            udp_refuse_dns(slirp, ip, uh, len);' "$PATCH" || { echo "FAIL: nothing calls it"; exit 1; }
T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
awk '/^\+static void udp_refuse_dns/ { on = 1 } on { print } on && /^\+}/ { exit }' "$PATCH" | sed 's/^+//' > "$T/refuse.h"
cat > "$T/test.c" <<'TEST'
#include <arpa/inet.h>
#include <assert.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef int Slirp;
struct ip { struct in_addr ip_src, ip_dst; };
struct udphdr { uint16_t uh_sport, uh_dport, uh_ulen, uh_sum; };
struct mbuf { char *m_data; int m_len; char room[2048]; };
#define IF_MAXLINKHDR 64
#define IPTOS_LOWDELAY 0x10
#define M_FREEROOM(m) ((int)((m)->room + sizeof((m)->room) - (m)->m_data))
static struct mbuf out;
static int sent;
static struct sockaddr_in from, to;
static struct mbuf *m_get(Slirp *s) { memset(&out, 0, sizeof(out)); out.m_data = out.room; return &out; }
static void m_free(struct mbuf *m) { }
static int udp_output(void *so, struct mbuf *m, struct sockaddr_in *saddr, struct sockaddr_in *daddr, int tos)
{
    sent++;
    from = *saddr;
    to = *daddr;
    return 0;
}
#include "refuse.h"

static const uint8_t query[] = { 0x12, 0x34, 0x01, 0x00, 0, 1, 0, 0, 0, 0, 0, 0,
                                 5, 'a', 'p', 'p', 'l', 'e', 3, 'c', 'o', 'm', 0, 0, 1, 0, 1 };

static int run(const uint8_t *q, int n)
{
    static uint8_t frame[sizeof(struct udphdr) + 4096];
    struct udphdr *uh = (struct udphdr *)frame;
    struct ip ip = { .ip_src.s_addr = htonl(0x0a00020f), .ip_dst.s_addr = htonl(0x0a000203) };
    Slirp slirp = 0;

    uh->uh_sport = htons(4242);
    uh->uh_dport = htons(53);
    memcpy(uh + 1, q, n);
    sent = 0;
    udp_refuse_dns(&slirp, &ip, uh, sizeof(*uh) + n);
    return sent;
}

int main(void)
{
    uint8_t bad[sizeof(query)];

    assert(run(query, sizeof(query)) == 1);
    const uint8_t *r = (const uint8_t *)out.m_data;
    assert(r[0] == 0x12 && r[1] == 0x34);             /* its ID */
    assert(r[2] == 0x81 && r[3] == 0x83);             /* a response, RD as asked, RA, NXDOMAIN */
    assert(r[4] == 0 && r[5] == 1 && r[6] == 0 && r[7] == 0 && r[8] == 0 && r[9] == 1 && r[10] == 0 && r[11] == 0);
    assert(!memcmp(r + 12, query + 12, sizeof(query) - 12));   /* the question */
    assert(out.m_len == (int)sizeof(query) + 33);
    const uint8_t *soa = r + sizeof(query);
    assert(soa[0] == 0 && soa[1] == 0 && soa[2] == 6 && soa[4] == 1);    /* root, SOA, IN */
    assert(!soa[5] && !soa[6] && !soa[7] && !soa[8]);                    /* TTL 0: nothing caches it */
    assert(soa[10] == 22);
    assert(from.sin_addr.s_addr == htonl(0x0a000203) && ntohs(from.sin_port) == 53);
    assert(to.sin_addr.s_addr == htonl(0x0a00020f) && ntohs(to.sin_port) == 4242);

    /* Dropped: a response, another opcode, two questions, a compressed name, and any truncation. */
    memcpy(bad, query, sizeof(bad)); bad[2] = 0x81; assert(!run(bad, sizeof(bad)));
    memcpy(bad, query, sizeof(bad)); bad[2] = 0x09; assert(!run(bad, sizeof(bad)));
    memcpy(bad, query, sizeof(bad)); bad[5] = 2; assert(!run(bad, sizeof(bad)));
    memcpy(bad, query, sizeof(bad)); bad[12] = 0xc0; assert(!run(bad, sizeof(bad)));
    for (unsigned n = 0; n < sizeof(query); n++) {
        assert(!run(query, n));
    }
    /* A label running past the datagram. */
    memcpy(bad, query, sizeof(bad)); bad[12] = 63; assert(!run(bad, sizeof(bad)));
    puts("PASS: restricted DNS answers a query at once with a zero-TTL NXDOMAIN, and drops anything else");
    return 0;
}
TEST
cc -std=gnu11 -Wall -Werror -fsanitize=address,undefined -fno-sanitize-recover=all -I"$T" "$T/test.c" -o "$T/test"
"$T/test"
