/* MIT; Copyright (c) 2026 j0shua-SYSON. Adapted from S5LBox
 * 6f203ba550b49afadee008c7eb55373a838eed33 core/src/soc/mbx.c.
 * Preserve licenses/S5LBox-MIT.txt with distributions.
 * Experimental independently captured N72 7E18 black-fill family only.
 * See docs/ipod/mbx-fill.md. Not a general MBX renderer. */
#include "hw/arm/mbx_fill.h"
#include <string.h>

#define RING_BASE 0xa00000u
#define RING_SIZE 0x10000u
#define HEADER 0xa0060500u
#define SUBMIT 0xf0000000u
#define PAGES 150u /* Captured BGRA8 320x480, stride 1280. */

static uint32_t load_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 |
           (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static MBXFillResult reject(const char **why, const char *reason)
{
    if (why) {
        *why = reason;
    }
    return MBX_FILL_REJECTED;
}

void mbx_fill_reset(MBXFillState *s)
{
    memset(s, 0, sizeof *s);
}

MBXFillResult mbx_fill_write(MBXFillState *s, const MBXFillBus *bus,
                           uint32_t off, uint32_t value, const char **why)
{
    if (off >= 0x1000 && off <= 0x101c && !(off & 3)) {
        s->roots[(off - 0x1000) / 4] = value;
        return MBX_FILL_IGNORED;
    }
    if (off < RING_BASE || off >= RING_BASE + RING_SIZE || (off & 3)) {
        return MBX_FILL_IGNORED;
    }
    s->ring[(off - RING_BASE) / 4] = value;
    if (value == HEADER) {
        if (!s->pending_count) {
            s->pending_offset = off;
            s->pending_mask = 0;
        }
        if (s->pending_count < 2) {
            s->pending_count++;
        }
    }
    if (off != RING_BASE || value != SUBMIT) {
        if (s->pending_count && off >= s->pending_offset &&
            off - s->pending_offset < 64) {
            s->pending_mask |= 1u << ((off - s->pending_offset) / 4);
        }
        return MBX_FILL_IGNORED;
    }
    uint32_t count = s->pending_count, head = s->pending_offset;
    uint32_t mask = s->pending_mask;
    s->pending_count = 0;
    s->pending_mask = 0;
    if (count != 1 || mask != 0xffff || head < RING_BASE ||
        head > RING_BASE + RING_SIZE - 64) {
        return reject(why, "requires one freshly written complete packet");
    }
    const uint32_t *w = s->ring + (head - RING_BASE) / 4;
    if ((w[0] != HEADER && w[0] != SUBMIT) || w[2] != 0x94060500 ||
        w[3] != 0 || w[4] != 0x30000000 || w[5] != 0x60800200 ||
        w[6] != 0x8000f0f0 || w[7] != 0xff000000 || w[8] != 0 ||
        w[9] != 0x014001e0) {
        return reject(why, "not the measured N72 full-screen black fill");
    }
    for (unsigned i = 10; i < 16; i++) {
        if (w[i] != 0x70000000) {
            return reject(why, "unsupported packet tail");
        }
    }
    if (!bus || !bus->host_ram || !bus->write32 || (w[1] & 0xfff) ||
        (uint64_t)w[1] + PAGES * 4096u > 8u * 0x400000u) {
        return reject(why, "target outside measured eight-root GART");
    }
    /* Resolve every destination before the first write. Do not re-read PTEs
     * during commit: a destination may alias the page table itself. */
    uint32_t physical[PAGES];
    for (unsigned i = 0; i < PAGES; i++) {
        uint32_t va = w[1] + i * 4096u;
        uint32_t pa = va;
        if (bus->mmu_enabled) {
            uint32_t root = s->roots[va >> 22];
            uint32_t pte_off = ((va >> 12) & 1023u) * 4;
            if (!root || (root & 0xfff) || root > UINT32_MAX - pte_off) {
                return reject(why, "invalid GART root");
            }
            const uint8_t *entry = bus->host_ram(bus->ctx, root + pte_off, 4);
            if (!entry) {
                return reject(why, "PTE is outside writable plain RAM");
            }
            pa = load_le32(entry);
        }
        if (!pa || (pa & 0xfff) || !bus->host_ram(bus->ctx, pa, 4096)) {
            return reject(why, "target is outside writable plain RAM");
        }
        physical[i] = pa;
    }
    for (unsigned i = 0; i < PAGES; i++) {
        for (unsigned j = 0; j < 4096; j += 4) {
            bus->write32(bus->ctx, physical[i] + j, 0xff000000);
        }
    }
    return MBX_FILL_DONE;
}
