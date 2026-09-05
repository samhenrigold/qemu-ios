#include "hw/arm/ipod_touch_fmss.h"
#include "hw/arm/ipod_touch_guard.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/error-report.h"
#include "system/runstate.h"
#include "exec/address-spaces.h"
#include <sys/mman.h>
#include <sys/stat.h>

/*
 * Cached env lookups.
 *
 * fmss_load_page called getenv() four to five times PER 4 KB PAGE (FMSS_RTRACE
 * twice, FMSS_ERASE via fmss_block_is_erased, FMSS_USEDSPARE, FMSS_BASESPARE),
 * and fmss_store_page two more per written page. Each call is a linear scan of
 * environ, and all of it runs synchronously on the vCPU thread inside the MMIO
 * handler - so it is guest stall, not background work. A 20 MB app launch is
 * roughly 5,000 pages, i.e. 20,000+ environ scans.
 *
 * Same static-cached pattern already used by mbx_tracing(), amc_trace() and
 * mt_trace(). Purely mechanical: no semantic change, since none of these are
 * meant to be togglable mid-run.
 */
#define FMSS_ENV_FLAG(fn, name)                                               \
    static bool fn(void)                                                      \
    {                                                                         \
        static int on = -1;                                                   \
        if (on < 0) {                                                         \
            on = getenv(name) != NULL;                                        \
        }                                                                     \
        return on;                                                            \
    }

FMSS_ENV_FLAG(fmss_rtrace,    "FMSS_RTRACE")
FMSS_ENV_FLAG(fmss_erase_on,  "FMSS_ERASE")
FMSS_ENV_FLAG(fmss_usedspare, "FMSS_USEDSPARE")
FMSS_ENV_FLAG(fmss_basespare, "FMSS_BASESPARE")
FMSS_ENV_FLAG(fmss_legacy_on, "FMSS_LEGACY")
FMSS_ENV_FLAG(fmss_dump_on,   "FMSS_DUMP")
FMSS_ENV_FLAG(fmss_physical,  "FMSS_PHYSICAL")
FMSS_ENV_FLAG(fmss_trace_on,  "FMSS_TRACE")
FMSS_ENV_FLAG(fmss_stats_on,  "FMSS_STATS")

/*
 * FMSS_STATS: how many pages the guest reads, and how long the host spends
 * serving them.
 *
 * The page-in cost of a large app is the leading hypothesis for why big apps
 * open with no zoom animation while small ones animate, so it has to be
 * measured rather than assumed. Every read runs synchronously on the vCPU
 * thread inside the MMIO handler, so time spent here is guest stall.
 *
 * Reported every 256 reads to keep the volume low enough to correlate against
 * a launch window without perturbing what is being measured.
 */
static struct {
    uint64_t reads, base, overlay, recall, blank, ns;
    /* Stores that landed on coordinates this session had already programmed --
     * the only way the phys_pages read-cache could shadow a newer write. See
     * the note in fmss_store_page. */
    uint64_t shadowed;
} fmss_stats;

static void fmss_stats_report(void)
{
    if (!fmss_stats_on()) {
        return;
    }
    if ((fmss_stats.reads & 0xff) != 0) {
        return;
    }
    fprintf(stderr,
            "[FMSS] reads=%llu base=%llu ovl=%llu recall=%llu blank=%llu "
            "shadowed=%llu read_ms=%.1f\n",
            (unsigned long long)fmss_stats.reads,
            (unsigned long long)fmss_stats.base,
            (unsigned long long)fmss_stats.overlay,
            (unsigned long long)fmss_stats.recall,
            (unsigned long long)fmss_stats.blank,
            (unsigned long long)fmss_stats.shadowed,
            fmss_stats.ns / 1.0e6);
}

static uint8_t find_bit_index(uint8_t num) {
    int index = 0;
    while (num > 1) {
        num >>= 1;
        index++;
    }
    return index;
}

static void write_chip_info(IPodTouchFMSSState *s)
{
    uint32_t chipid[] = { 0xb614d5ad, 0xb614d5ad, 0xb614d5ad, 0xb614d5ad };
    cpu_physical_memory_write(s->reg_cinfo_target_addr, &chipid, 0x10);
}

/*
 * Erased-block bookkeeping.
 *
 * The FMSS driver only ever issues read (csgenrc 0xa01) and write (0xa02)
 * through the trigger register, so a block erase is never visible to us -- but
 * it certainly happens, because the FTL rewrites blocks that are fully
 * populated in the base image while only programming a handful of their pages.
 * Without modelling the erase, the pages the FTL did not rewrite still read
 * back as the base image's *old* contents instead of as erased flash, so on
 * the next boot the FTL's scan finds stale valid-looking data where it expects
 * clean pages.
 *
 * NAND cannot program a page without erasing its block first, so we can infer
 * the erase from the write: the first time a block is programmed in the
 * overlay (and again whenever a page already present in the overlay is
 * reprogrammed) the whole block must have been erased just beforehand. We
 * record that as a marker file next to the pages so it survives a reboot, and
 * reads of unwritten pages in a marked block return erased flash rather than
 * falling through to the base image.
 */
static gpointer fmss_block_key(uint32_t cs, uint32_t block)
{
    return GUINT_TO_POINTER((cs << 24) | block);
}

static void fmss_block_marker_path(IPodTouchFMSSState *s, uint32_t cs,
                                   uint32_t block, char *buf, size_t len)
{
    snprintf(buf, len, "%s/cs%d/blk%u.erased", s->nand_overlay, cs, block);
}

static bool fmss_block_is_erased(IPodTouchFMSSState *s, uint32_t cs, uint32_t block)
{
    char marker[1152];

    if (!s->nand_overlay || !fmss_erase_on()) {
        return false;
    }
    if (!s->erased_blocks) {
        s->erased_blocks = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    if (g_hash_table_contains(s->erased_blocks, fmss_block_key(cs, block))) {
        return true;
    }
    fmss_block_marker_path(s, cs, block, marker, sizeof(marker));
    if (g_file_test(marker, G_FILE_TEST_EXISTS)) {
        g_hash_table_add(s->erased_blocks, fmss_block_key(cs, block));
        return true;
    }
    return false;
}

/* Host persistence failure is fatal to this session, even after reset. */
static bool fmss_io_failed;

bool ipod_touch_fmss_io_failed(void)
{
    return qatomic_read(&fmss_io_failed);
}

/* Stop before acknowledging writes that did not reach the host filesystem. */
static bool fmss_io_error(const char *path, int err)
{
    qatomic_set(&fmss_io_failed, true);
    error_report("FMSS: cannot persist %s: %s; VM stopped", path, strerror(err));
    vm_stop(RUN_STATE_IO_ERROR);
    return false;
}

/* Drop every overlay page of a block and mark it erased. */
static bool fmss_erase_block(IPodTouchFMSSState *s, uint32_t cs, uint32_t block)
{
    char path[1152], tmp[1200];
    uint32_t first = block * NAND_PAGES_PER_BLOCK;

    /*
     * Marker FIRST. The pages and the marker together say "this block is
     * erased"; with the pages removed and no marker, reads fall through to the
     * pristine BASE image and serve its stale contents in the middle of a live
     * volume -- valid-looking bytes, which is the worst kind of wrong. Writing
     * the marker first can only cost a block that reads back as erased, which
     * is what the guest asked for anyway.
     */
    snprintf(path, sizeof(path), "%s/cs%d", s->nand_overlay, cs);
    if (g_mkdir_with_parents(path, 0755) != 0) {
        return fmss_io_error(path, errno);
    }
    fmss_block_marker_path(s, cs, block, path, sizeof(path));
    snprintf(tmp, sizeof(tmp), "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return fmss_io_error(path, errno);
    }
    int err = 0;
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        err = errno;
    }
    if (fclose(f) != 0 && !err) {
        err = errno;
    }
    if (err) {
        remove(tmp);
        return fmss_io_error(path, err);
    }
    if (rename(tmp, path) != 0) {
        err = errno;
        remove(tmp);
        return fmss_io_error(path, err);
    }

    /* ponytail: multi-file erase can stop partway; journal blocks if atomic
     * crash recovery is needed. Never acknowledge a partial erase. */
    for (uint32_t p = first; p < first + NAND_PAGES_PER_BLOCK; p++) {
        snprintf(path, sizeof(path), "%s/cs%d/%u.page", s->nand_overlay, cs, p);
        if (remove(path) != 0 && errno != ENOENT) {
            return fmss_io_error(path, errno);
        }
        if (s->overlay_pages) {
            g_hash_table_remove(s->overlay_pages, fmss_block_key(cs, p));
        }
    }

    if (!s->erased_blocks) {
        s->erased_blocks = g_hash_table_new(g_direct_hash, g_direct_equal);
    }
    g_hash_table_add(s->erased_blocks, fmss_block_key(cs, block));
    return true;
}

/*
 * Load one physical page (data + spare) into the caller buffers. When a
 * writable overlay is configured it takes precedence over the read-only base
 * image (copy-on-write); a page present in neither reads back as a blank/erased
 * page, exactly as the original read-only model did.
 */
/* Record a page programmed at its physical address, so it reads back there. */
static void fmss_remember_physical(IPodTouchFMSSState *s, uint32_t cs,
                                   uint32_t page_nr, const uint8_t *data,
                                   const uint8_t *spare)
{
    if (!s->phys_pages) {
        s->phys_pages = g_hash_table_new_full(g_direct_hash, g_direct_equal,
                                              NULL, g_free);
    }

    uint8_t *slot = g_malloc(NAND_BYTES_PER_PAGE + NAND_BYTES_PER_SPARE);
    memcpy(slot, data, NAND_BYTES_PER_PAGE);
    memcpy(slot + NAND_BYTES_PER_PAGE, spare, NAND_BYTES_PER_SPARE);
    g_hash_table_insert(s->phys_pages, fmss_block_key(cs, page_nr), slot);
}

static bool fmss_recall_physical(IPodTouchFMSSState *s, uint32_t cs,
                                 uint32_t page_nr, uint8_t *data, uint8_t *spare)
{
    if (!s->phys_pages) {
        return false;
    }

    const uint8_t *slot = g_hash_table_lookup(s->phys_pages,
                                              fmss_block_key(cs, page_nr));
    if (!slot) {
        return false;
    }
    memcpy(data, slot, NAND_BYTES_PER_PAGE);
    memcpy(spare, slot + NAND_BYTES_PER_PAGE, NAND_BYTES_PER_SPARE);
    return true;
}

static void fmss_load_page_inner(IPodTouchFMSSState *s, uint32_t cs,
                                 uint32_t page_nr, uint8_t *data,
                                 uint8_t *spare);
static void fmss_fix_generated_free_pool(IPodTouchFMSSState *s, uint32_t cs,
                                         uint32_t page, uint8_t *data,
                                         const uint8_t *spare);

/*
 * Time every page read. Split from the body so the accounting cannot miss one
 * of the several early returns -- the blank-page and recall paths both return
 * before the file is ever opened.
 */
static void fmss_load_page(IPodTouchFMSSState *s, uint32_t cs, uint32_t page_nr,
                           uint8_t *data, uint8_t *spare)
{
    int64_t t0;

    if (!fmss_stats_on()) {
        fmss_load_page_inner(s, cs, page_nr, data, spare);
        fmss_fix_generated_free_pool(s, cs, page_nr, data, spare);
        return;
    }
    t0 = qemu_clock_get_ns(QEMU_CLOCK_HOST);
    fmss_load_page_inner(s, cs, page_nr, data, spare);
    fmss_fix_generated_free_pool(s, cs, page_nr, data, spare);
    fmss_stats.ns += qemu_clock_get_ns(QEMU_CLOCK_HOST) - t0;
    fmss_stats.reads++;
    fmss_stats_report();
}

/*
 * Map a packed base image, if that is what nand= points at.
 *
 * Serving the base NAND from a directory costs an fopen/fread/fclose per 4 KB
 * the guest reads -- three syscalls for every page of every file it opens, on
 * the vCPU thread. Packed (imgtools/pack_nand.py) the whole base is one
 * mapping and a read is a memcpy. Falls back silently: a directory still
 * works, just slower.
 */
static void fmss_try_map_packed(IPodTouchFMSSState *s)
{
    static const char magic[8] = "ITNAND01";
    struct stat st;
    const uint8_t *p;
    uint32_t page_size, spare_size;
    size_t index_bytes;
    int fd;

    if (!s->nand_path || stat(s->nand_path, &st) != 0 || !S_ISREG(st.st_mode)) {
        return;                       /* a directory, or nothing: legacy path */
    }

    fd = open(s->nand_path, O_RDONLY);
    if (fd < 0) {
        return;
    }
    p = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);                        /* the mapping keeps its own reference */
    if (p == MAP_FAILED) {
        return;
    }

    if ((size_t)st.st_size < 24 || memcmp(p, magic, sizeof(magic)) != 0) {
        fprintf(stderr, "[fmss] %s is not a packed NAND image\n", s->nand_path);
        munmap((void *)p, st.st_size);
        return;
    }

    page_size = ldl_le_p(p + 8);
    spare_size = ldl_le_p(p + 12);
    if (page_size != NAND_BYTES_PER_PAGE || spare_size != NAND_BYTES_PER_SPARE) {
        fprintf(stderr, "[fmss] %s has %u+%u byte pages, expected %u+%u\n",
                s->nand_path, page_size, spare_size,
                NAND_BYTES_PER_PAGE, NAND_BYTES_PER_SPARE);
        munmap((void *)p, st.st_size);
        return;
    }

    s->packed_num_cs = ldl_le_p(p + 16);
    s->packed_pages_per_cs = ldl_le_p(p + 20);
    index_bytes = (size_t)s->packed_num_cs * s->packed_pages_per_cs * 4;

    /*
     * Every bound below comes out of the file, so the file has to be checked
     * against itself. Only the 24-byte header was validated before: a
     * truncated or half-written image (an interrupted copy, a full disk) still
     * passed the magic check, and the first guest page read then indexed past
     * the mapping and took SIGBUS -- the whole app, during boot, with nothing
     * to suggest the image was the problem.
     */
    if (index_bytes > (size_t)st.st_size - 24) {
        fprintf(stderr, "[fmss] packed NAND %s is truncated (index wants %zu of "
                "%zu bytes); ignoring it\n", s->nand_path, index_bytes,
                (size_t)st.st_size - 24);
        munmap(p, st.st_size);
        return;
    }

    s->packed = p;
    s->packed_size = st.st_size;
    s->packed_index = (const uint32_t *)(p + 24);
    s->packed_records = p + 24 + index_bytes;
    s->packed_record_count = ((size_t)st.st_size - 24 - index_bytes)
                             / (NAND_BYTES_PER_PAGE + NAND_BYTES_PER_SPARE);

    fprintf(stderr, "[fmss] mapped packed NAND %s (%u cs x %u pages, %zu MiB)\n",
            s->nand_path, s->packed_num_cs, s->packed_pages_per_cs,
            s->packed_size >> 20);
}

/* True when the packed image carries this page; fills data and spare if so. */
static bool fmss_packed_page(IPodTouchFMSSState *s, uint32_t cs,
                             uint32_t page_nr, uint8_t *data, uint8_t *spare)
{
    const uint8_t *rec;
    uint32_t slot;

    if (!s->packed || cs >= s->packed_num_cs ||
        page_nr >= s->packed_pages_per_cs) {
        return false;
    }

    slot = ldl_le_p(&s->packed_index[(size_t)cs * s->packed_pages_per_cs + page_nr]);
    if (slot == 0) {
        return false;                 /* absent, i.e. erased */
    }
    /* The slot number is file data too, and it is the one that becomes a
     * pointer. Out of range means the image disagrees with itself. */
    if (slot > s->packed_record_count) {
        qemu_log_mask(LOG_GUEST_ERROR, "[fmss] packed slot %u past the end of "
                      "the image (%zu records); page treated as erased\n",
                      slot, s->packed_record_count);
        return false;
    }

    rec = s->packed_records +
          (size_t)(slot - 1) * (NAND_BYTES_PER_PAGE + NAND_BYTES_PER_SPARE);
    memcpy(data, rec, NAND_BYTES_PER_PAGE);
    memcpy(spare, rec + NAND_BYTES_PER_PAGE, NAND_BYTES_PER_SPARE);
    return true;
}

/*
 * Index the overlay once, so misses never reach the filesystem.
 *
 * Built by listing the directories rather than by remembering what this session
 * wrote, because the overlay persists across runs -- the pages that matter most
 * are the ones a previous boot left behind.
 */
static void fmss_overlay_index(IPodTouchFMSSState *s)
{
    uint32_t cs;

    if (s->overlay_indexed || !s->nand_overlay) {
        return;
    }
    s->overlay_indexed = true;
    s->overlay_pages = g_hash_table_new(g_direct_hash, g_direct_equal);

    for (cs = 0; cs < 4; cs++) {
        char dir[1088];
        const gchar *name;
        GDir *d;

        snprintf(dir, sizeof(dir), "%s/cs%u", s->nand_overlay, cs);
        d = g_dir_open(dir, 0, NULL);
        if (!d) {
            continue;
        }
        while ((name = g_dir_read_name(d))) {
            char *end;
            unsigned long page = strtoul(name, &end, 10);

            if (end != name && g_str_equal(end, ".page")) {
                g_hash_table_add(s->overlay_pages,
                                 fmss_block_key(cs, (uint32_t)page));
            }
        }
        g_dir_close(d);
    }
}

static bool fmss_overlay_has(IPodTouchFMSSState *s, uint32_t cs, uint32_t page_nr)
{
    fmss_overlay_index(s);
    return s->overlay_pages &&
           g_hash_table_contains(s->overlay_pages, fmss_block_key(cs, page_nr));
}

static void fmss_load_page_inner(IPodTouchFMSSState *s, uint32_t cs,
                                 uint32_t page_nr, uint8_t *data,
                                 uint8_t *spare)
{
    char filename[1088];
    FILE *f = NULL;
    bool from_overlay = false;

    /*
     * A page programmed in this session reads back as programmed, whatever the
     * base image holds at that physical address. Without this the FTL reads its
     * own freshly written page and gets an unrelated file, so anything written
     * and then read before the next boot -- an .ipa staged by installd, most
     * visibly -- comes back as garbage.
     */
    if (fmss_recall_physical(s, cs, page_nr, data, spare)) {
        fmss_stats.recall++;
        if (fmss_rtrace()) {
            printf("RP cs=%u page=%u\n", cs, page_nr); fflush(stdout);
        }
        return;
    }

    if (s->nand_overlay && fmss_overlay_has(s, cs, page_nr)) {
        snprintf(filename, sizeof(filename), "%s/cs%d/%d.page", s->nand_overlay, cs, page_nr);
        f = fopen(filename, "rb");
        from_overlay = (f != NULL);
        if (from_overlay) { fmss_stats.overlay++; }
        if (f && fmss_rtrace()) {
            printf("RH cs=%u page=%u\n", cs, page_nr); fflush(stdout);
        }
    }
    if (!f && !fmss_block_is_erased(s, cs, page_nr / NAND_PAGES_PER_BLOCK)) {
        if (s->packed) {
            if (fmss_packed_page(s, cs, page_nr, data, spare)) {
                fmss_stats.base++;
                /* The two diagnostics below only ever rewrite the spare of a
                 * base-image page, and both are off unless asked for. */
                if (fmss_usedspare() &&
                    ((uint32_t *)spare)[2] == 0x00FF00FF) {
                    ((uint32_t *)spare)[2] = 0xFFFF40FF;
                }
                return;
            }
            fmss_stats.blank++;
            memset(data, 0, NAND_BYTES_PER_PAGE);
            memset(spare, 0, NAND_BYTES_PER_SPARE);
            ((uint32_t *)spare)[2] = 0x00FF00FF; /* clean/erased marker */
            return;
        }
        snprintf(filename, sizeof(filename), "%s/cs%d/%d.page", s->nand_path, cs, page_nr);
        f = fopen(filename, "rb");
        if (f) { fmss_stats.base++; }
    }

    if (!f) {
        fmss_stats.blank++;
        memset(data, 0, NAND_BYTES_PER_PAGE);
        memset(spare, 0, NAND_BYTES_PER_SPARE);
        ((uint32_t *)spare)[2] = 0x00FF00FF; /* clean/erased marker */
        return;
    }
    /* A short read is tolerated, but the tail MUST be zeroed: these buffers are
     * reused across pages, so leaving it would serve the previous page's bytes
     * as the missing half of this one -- a plausible-looking frankenpage in the
     * middle of a live filesystem. */
    size_t got = fread(data, 1, NAND_BYTES_PER_PAGE, f);
    if (got != NAND_BYTES_PER_PAGE) {
        memset(data + got, 0, NAND_BYTES_PER_PAGE - got);
    }
    got = fread(spare, 1, NAND_BYTES_PER_SPARE, f);
    if (got != NAND_BYTES_PER_SPARE) {
        memset(spare + got, 0, NAND_BYTES_PER_SPARE - got);
    }
    fclose(f);

    /* Diagnostic: every page of the reference image carries the emulator's own
     * "clean" marker in its spare, so the guest FTL believes the whole device
     * is free and happily allocates new writes on top of pages that are still
     * live. Claim occupied pages are programmed instead, and see whether the
     * allocator then steers around them. */
    if (!from_overlay && fmss_usedspare()) {
        if (((uint32_t *)spare)[2] == 0x00FF00FF) {
            ((uint32_t *)spare)[2] = 0xFFFF40FF;
        }
    }

    /* Diagnostic: serve the base image's spare metadata even for overlay
     * pages, to test whether the persisted FTL metadata is what breaks the
     * next boot. */
    if (from_overlay && fmss_basespare()) {
        snprintf(filename, sizeof(filename), "%s/cs%d/%d.page", s->nand_path, cs, page_nr);
        FILE *bf = fopen(filename, "rb");
        memset(spare, 0, NAND_BYTES_PER_SPARE);
        if (bf) {
            if (fseek(bf, NAND_BYTES_PER_PAGE, SEEK_SET) == 0) {
                if (fread(spare, 1, NAND_BYTES_PER_SPARE, bf) != NAND_BYTES_PER_SPARE) { }
            }
            fclose(bf);
        } else {
            ((uint32_t *)spare)[2] = 0x00FF00FF;
        }
    }
}

/*
 * Where the *generated* NAND image puts a filesystem block.
 *
 * `nand=` is not a dump of real flash. It is a synthetic image built by laying
 * a single HFSX volume (SugarBowl5F138.N72OS, 4096-byte allocation blocks,
 * 128000 of them) out across the four chip-selects with a fixed formula, and
 * every page carries the same placeholder spare, so the guest FTL believes the
 * whole device is free. Reads work because the FTL's mapping for the pristine
 * image is exactly this formula.
 *
 * Writes do not, because the FTL is log-structured: it programs each page to a
 * freshly allocated physical page, which here is still holding another file's
 * data. Persisting at that physical address corrupts the image -- measurably
 * so: it rewrites system frameworks (CoreAudio, AudioCodecs, aosnotifyd, ...)
 * and never touches /private/var, which is the opposite of what a first boot
 * should do.
 *
 * The destination the FTL actually intends is in the spare: its first word is
 * the logical page number, and `logical = block + 3` -- the same +3 that
 * appears in the layout formula. That was pinned down by content-anchoring 22
 * written pages against the pristine volume, and confirmed end to end: placing
 * the pages at their logical blocks yields a volume whose only changes are
 * /private/var/run/{configd.pid,lockdown,SCHelper,utmpx}, SpringBoard's and
 * mobile installation's plists, and Library/Keychains/TrustStore.sqlite3.
 *
 * So translate back into the generated layout on the way out. Returns false
 * for logical numbers outside the volume, which are FTL bookkeeping rather
 * than filesystem blocks and are meaningless once the indirection is undone.
 */
/*
 * How much of the flash the volume actually covers.
 *
 * The layout formula below is periodic and addresses erase blocks 2..4094 of
 * every chip-select -- the whole 8 GiB the part table gives chip id
 * 0xb614d5ad -- but a given image only *contains* a volume of a particular
 * size, and a write past its end is FTL bookkeeping rather than a filesystem
 * block. That boundary is a property of the image, so read it out of the
 * image: logical block 2 is the GPT's one-entry partition array, and the
 * entry's ending LBA is the last block of disk0s1.
 *
 * Falls back to the original image's 128000 if the GPT cannot be read, which
 * is what this was hardcoded to before.
 */
#define NAND_DEFAULT_TOTAL_BLOCKS 128000
#define NAND_MAX_TOTAL_BLOCKS     2090000  /* keeps eb clear of block 4095 */

static uint32_t fmss_total_blocks(IPodTouchFMSSState *s)
{
    char path[1088];
    uint8_t ent[NAND_BYTES_PER_PAGE], spare[NAND_BYTES_PER_SPARE];
    FILE *f;
    bool got = false;

    if (s->total_blocks) {
        return s->total_blocks;
    }
    s->total_blocks = NAND_DEFAULT_TOTAL_BLOCKS;

    /* logical 2 is cs2 page 256 under the formula below */
    if (s->packed) {
        got = fmss_packed_page(s, 2, 256, ent, spare);
    } else {
        snprintf(path, sizeof(path), "%s/cs2/256.page", s->nand_path);
        f = fopen(path, "rb");
        if (f) {
            got = fread(ent, 1, 0x30, f) == 0x30;
            fclose(f);
        }
    }
    if (got) {
        uint64_t start = ldq_le_p(ent + 0x20);
        uint64_t end = ldq_le_p(ent + 0x28);
        if (start == 3 && end > start && end - start < NAND_MAX_TOTAL_BLOCKS) {
            s->total_blocks = end - start + 1;
            s->total_blocks_from_gpt = true;
        }
    }
    return s->total_blocks;
}

/* The legacy generated image has an identity block map (LBN n -> VBN n+1),
 * but copied a factory free list containing VBNs 3..22. Those blocks already
 * contain system files. After a large install, phys_pages correctly returns
 * newly programmed IPA data there, corrupting reads of the original files.
 * Keep the pool outside the entire GPT volume, including its currently free
 * filesystem blocks. One VBN is 1024 pages: four chips, two 128-page planes.
 *
 * This is a compatibility correction to the known synthetic FTL context, not
 * a change to real NAND behavior. Physical images and updated/live contexts
 * are left alone. The image on disk is unchanged; reset reapplies the same
 * pool while the generated logical write-back preserves the filesystem.
 */
static void fmss_fix_generated_free_pool(IPodTouchFMSSState *s, uint32_t cs,
                                         uint32_t page, uint8_t *data,
                                         const uint8_t *spare)
{
    if (cs != 3 || page != 255 || fmss_physical() || spare[9] != 0x43 ||
        ldl_le_p(data) != 0 || ldl_le_p(data + 4) != 0 ||
        ldl_le_p(data + 8) != 20 || lduw_le_p(data + 12) != 0 ||
        ldl_le_p(data + 2040) != 0x46560000 ||
        ldl_le_p(data + 2044) != 0xb9a9ffff) {
        return;
    }
    for (unsigned i = 0; i < 20; i++) {
        if (lduw_le_p(data + 14 + i * 2) != i + 3) {
            return;
        }
    }
    uint32_t first = MAX(23, (fmss_total_blocks(s) + 3 + 1023) / 1024 + 1);
    if (!s->total_blocks_from_gpt) {
        fmss_io_error("generated NAND free pool requires a valid GPT", EINVAL);
        return;
    }
    /* Leave the reserved final physical erase block outside the pool. */
    if (first + 20 > 2047) {
        fmss_io_error("generated NAND has no room for a disjoint FTL free pool", EINVAL);
        return;
    }
    for (unsigned i = 0; i < 20; i++) {
        stw_le_p(data + 14 + i * 2, first + i);
    }
    fprintf(stderr, "[fmss] generated FTL free pool %u..%u (outside %u filesystem pages)\n",
            first, first + 19, fmss_total_blocks(s));
}

static bool fmss_generated_layout(IPodTouchFMSSState *s, uint32_t logical,
                                  uint32_t *cs, uint32_t *page)
{
    if (logical < 3 || (logical - 3) >= fmss_total_blocks(s)) {
        return false;
    }
    /* logical == block + 3, so these are just its quotient and remainder */
    uint32_t r = logical / 4;
    uint32_t eb = 2 * (r / 256) + 2 + (r % 2);
    *cs = logical % 4;
    *page = eb * 128 + (r % 256) / 2;
    return true;
}

/* Store one physical page (data + spare) into the overlay, atomically. */
static bool fmss_store_page(IPodTouchFMSSState *s, uint32_t cs, uint32_t page_nr,
                            const uint8_t *data, const uint8_t *spare)
{
    char dir[1088], filename[1152], tmp[1200];
    uint32_t block = page_nr / NAND_PAGES_PER_BLOCK;

    snprintf(dir, sizeof(dir), "%s/cs%d", s->nand_overlay, cs);
    if (g_mkdir_with_parents(dir, 0755) != 0) {
        return fmss_io_error(dir, errno);
    }
    snprintf(filename, sizeof(filename), "%s/%d.page", dir, page_nr);
    snprintf(tmp, sizeof(tmp), "%s/.%d.page.tmp", dir, page_nr);

    /*
     * The rename below is what makes a page store atomic, but a rename only
     * orders the *name*: without an fsync the directory entry can reach the
     * host filesystem while the bytes behind it have not, so a host crash or a
     * SIGKILL leaves a short or empty N.page shadowing the good base-image page
     * underneath it. And an unchecked fwrite renames a truncated page over a
     * correct one even with no crash at all -- a full host disk was enough.
     * This is the guest's only durable storage; both cases lose user data.
     */
    FILE *f = fopen(tmp, "wb");
    if (!f) {
        return fmss_io_error(tmp, errno);
    }
    int err = 0;
    if (fwrite(data, 1, NAND_BYTES_PER_PAGE, f) != NAND_BYTES_PER_PAGE ||
        fwrite(spare, 1, NAND_BYTES_PER_SPARE, f) != NAND_BYTES_PER_SPARE ||
        fflush(f) != 0 || fsync(fileno(f)) != 0) {
        err = errno ? errno : EIO;
    }
    if (fclose(f) != 0 && !err) {
        err = errno;
    }
    if (err) {
        remove(tmp);
        return fmss_io_error(tmp, err);
    }

    /* Stage the new page before an inferred erase can remove old data. */
    if (fmss_erase_on() &&
        (!fmss_block_is_erased(s, cs, block) ||
         g_file_test(filename, G_FILE_TEST_EXISTS)) &&
        !fmss_erase_block(s, cs, block)) {
        remove(tmp);
        return false;
    }
    if (rename(tmp, filename) != 0) {
        err = errno;
        remove(tmp);
        return fmss_io_error(filename, err);
    }
    /*
     * DETECTION, not a fix. Reads consult phys_pages (pages the guest
     * programmed this session, remembered at the address it programmed
     * them to) BEFORE the overlay, and nothing invalidates an entry when a
     * later store lands on those same coordinates -- so in principle a
     * stale cached page can shadow a newer overlay write forever.
     *
     * Whether that is reachable depends on the FTL's allocation colliding
     * with fmss_generated_layout's relocation target, which is not obvious
     * either way. Removing the entry here would "fix" it and would ALSO
     * break the invariant the cache exists for -- the FTL must read back
     * what it programmed at an address, and a different logical block now
     * living there on disk is not that. So: count it, and only act if a
     * real session ever hits it. IT_FMSS_SHADOW=1 prints each one.
     */
    if (s->phys_pages &&
        g_hash_table_contains(s->phys_pages, fmss_block_key(cs, page_nr))) {
        fmss_stats.shadowed++;
        if (getenv("IT_FMSS_SHADOW")) {
            fprintf(stderr, "[fmss] store cs=%u page=%u lands on a page this "
                    "session programmed there (%llu so far)\n",
                    cs, page_nr, (unsigned long long)fmss_stats.shadowed);
        }
    }
    fmss_overlay_index(s);
    if (s->overlay_pages) {
        g_hash_table_add(s->overlay_pages, fmss_block_key(cs, page_nr));
    }
    return true;
}

/*
 * iBoot injects the bluetooth MAC address into the device tree node named by a
 * literal string it carries. On this board the node hangs off uart1, not
 * uart3, so the string has to be rewritten before iBoot walks the tree.
 *
 * The string's address is build-specific (5F138 has it at PA 0x0FF2206C, 7E18
 * at 0x0FF21324), so rather than hardcode it, search the window iBoot's img3
 * DATA payload is mapped into. The img3 payload is mapped at IBOOT_MEM_BASE
 * with file offset == PA - IBOOT_MEM_BASE, and the string occurs exactly once
 * in both builds' decrypted iBoot.
 */
#define IBOOT_SCAN_PA_START  0x0ff00000u
#define IBOOT_SCAN_LEN       0x00040000u   /* covers both builds' iBoot images */

/* One-shot: the DeviceTree node is patched in the iBoot image in RAM the first
 * time the guest reads a page. A reset reloads that RAM, so the latch has to be
 * re-armed from ipod_touch_fmss_reset() or the second boot runs unpatched. */
static bool iboot_bt_patched;

static void patch_iboot_bluetooth_node(void)
{
    static const char needle[] = "arm-io/uart3/bluetooth";
    static const char replace[] = "arm-io/uart1/bluetooth";

    if (iboot_bt_patched) {
        return;
    }
    iboot_bt_patched = true;

    g_autofree uint8_t *image = g_try_malloc(IBOOT_SCAN_LEN);
    if (!image) {
        return;
    }
    cpu_physical_memory_read(IBOOT_SCAN_PA_START, image, IBOOT_SCAN_LEN);

    for (size_t i = 0; i + sizeof(needle) <= IBOOT_SCAN_LEN; i++) {
        if (memcmp(image + i, needle, sizeof(needle)) != 0) {
            continue;
        }
        uint32_t pa = IBOOT_SCAN_PA_START + i;
        cpu_physical_memory_write(pa, replace, strlen(replace));
        if (getenv("IT_PATCH_DEBUG")) {
            printf("[IBOOT] bluetooth node string patched at PA 0x%08x\n", pa);
        }
        return;
    }

    printf("[IBOOT] bluetooth node string not found in iBoot; not patching\n");
}

/*
 * 2.1.1's kernel command line.
 *
 * 5F138 iBoot builds its boot_args from a runtime buffer at PA 0x0FF2A584 and
 * hands the result to XNU. The NOR nvram path is not a substitute: 7E18 iBoot
 * heap-panics on any NOR boot-args (see nor_set_boot_args()), so the nvram
 * atom is deliberately left alone, and with no poke the kernel comes up with
 * an empty command line -- no rd=disk0s1, so it cannot find its root device,
 * resets, and iBoot restarts. That is a silent boot loop: the Apple logo stays
 * up, "Boot Failure Count" climbs, and no kernel serial output ever appears.
 *
 * iBoot overwrites the buffer as it runs, hence the rewrite on every NAND read
 * rather than a once-only patch.
 *
 * The address is 5F138-specific, so this is skipped on the direct-iBoot
 * (3.1.3 / 7E18) path, which sets its command line via IT_BOOT_ARGS instead.
 */
#define IBOOT_5F138_BOOT_ARGS_PA  0x0ff2a584u

static void patch_iboot_boot_args(void)
{
    static const char boot_args[] =
        "kextlog=0xfff debug=0x8 cpus=1 rd=disk0s1 serial=1 pmu-debug=0x1 "
        "io=0xffff8fff debug-usb=0xffffffff amfi_allow_any_signature=1 -v "
        "zalloc_debug";

    if (getenv("IT_DIRECT_IBOOT")) {
        return;
    }

    cpu_physical_memory_write(IBOOT_5F138_BOOT_ARGS_PA, boot_args,
                              strlen(boot_args));
}

/*
 * Noticing that the home screen was rearranged.
 *
 * SpringBoard 3.1.3 publishes NOTHING when the user moves an icon.
 * -[SBIconModel saveIconState] writes the layout straight into CFPreferences
 * under the `iconState2` key and calls CFPreferencesAppSynchronize -- and 3.1.3
 * has no cfprefsd, so that synchronize is a direct file write from inside
 * SpringBoard's own process. There is no notification to observe, and the file
 * (/var/mobile/Library/Preferences/com.apple.springboard.plist) is outside
 * /var/mobile/Media, so AFC cannot reach it either. The host's only option used
 * to be re-reading the layout over sbservices every 15 seconds and hoping.
 *
 * But the write has to cross this device on its way to flash, and the key name
 * is literal ASCII in the plist whichever format CFPreferences chose. So sniff
 * the page: a programmed page carrying "iconState" is that file being
 * rewritten, and the host can re-read the layout the moment it lands.
 *
 * Deliberately a content match rather than resolving the plist's HFS+ catalog
 * extents once and filtering by page number. Extents have to be re-derived
 * whenever the file grows, moves, or the volume is rebuilt, and when they go
 * stale they go stale SILENTLY -- the signal simply stops, which looks exactly
 * like "the feature was never there". A byte match cannot rot that way.
 *
 * It is also allowed to be imprecise, because of what the signal is for: it
 * only tells the host to redo a read it already performs correctly on a timer.
 * A false positive costs one sbservices round trip; a false negative costs
 * nothing the existing poll does not already cover. That asymmetry is why the
 * cheap mechanism is the right one here.
 */
static uint64_t fmss_icon_state_writes;

uint64_t ipod_touch_fmss_icon_state_writes(void);

uint64_t ipod_touch_fmss_icon_state_writes(void)
{
    return qatomic_read(&fmss_icon_state_writes);
}

static void fmss_sniff_icon_state(const uint8_t *page)
{
    static const char needle[] = "iconState";

    if (memmem(page, NAND_BYTES_PER_PAGE, needle, sizeof(needle) - 1)) {
        qatomic_inc(&fmss_icon_state_writes);
    }
}

/* Four chips, 4096 erase blocks per chip. This is a sanity bound, not a
 * transfer size: real file writes exceed 512 pages in a single script. */
#define FMSS_MAX_WRITE_ENTRIES (4 * 4096 * NAND_PAGES_PER_BLOCK)

static bool fmss_write_dma_read(uint64_t addr, void *data, size_t len)
{
    MemoryRegionSection section = memory_region_find(get_system_memory(), addr, len);
    bool valid = section.mr && memory_region_is_ram(section.mr) &&
                 int128_eq(section.size, int128_make64(len));
    if (valid) {
        cpu_physical_memory_read(addr, data, len);
    }
    if (section.mr) {
        memory_region_unref(section.mr);
    }
    return valid || fmss_io_error("guest write DMA", EFAULT);
}

static void read_nand_pages(IPodTouchFMSSState *s)
{
    patch_iboot_boot_args();
    patch_iboot_bluetooth_node();

    int page_out_buf_ind = 0;
    for(int page_ind = 0; page_ind < s->reg_num_pages; page_ind++) {
        uint32_t page_nr = 0;
        uint32_t page_out_addr = 0;
        uint32_t cs = 0;
        cpu_physical_memory_read(s->reg_pages_in_addr + (page_ind * sizeof(uint32_t)), &page_nr, sizeof(uint32_t));
        cpu_physical_memory_read(s->reg_cs_buf_addr + (page_ind * sizeof(uint32_t)), &cs, sizeof(uint32_t));
        uint32_t og_cs = cs;
        cs = find_bit_index(cs);

        if(cs > 3) {
            /* The chip-select comes from a guest descriptor; a bad one skips
             * the page rather than aborting the whole process. */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "[fmss] invalid CS %u (original 0x%x, page %u); skipped\n",
                          cs, og_cs, page_nr);
            continue;
        }

        fmss_load_page(s, cs, page_nr, s->page_buffer, s->page_spare_buffer);

        // we write away the page in two parts, 2048 bytes first and then the other 2048 bytes.
        int write_buf_size = NAND_BYTES_PER_PAGE / 2;
        for(int i = 0; i < 2; i++) {
            cpu_physical_memory_read(s->reg_pages_out_addr + (page_out_buf_ind * sizeof(uint32_t)), &page_out_addr, sizeof(uint32_t));
            cpu_physical_memory_write(page_out_addr, s->page_buffer + i * write_buf_size, write_buf_size);
            page_out_buf_ind++;
        }

        /*
         * Finally, the spare -- 0xc bytes, NOT NAND_BYTES_PER_SPARE.
         *
         * The driver's array is 3 words per page (stride 0xc; the write path at
         * the bottom of this file reads exactly 0xc back). Writing the full
         * 64-byte spare at a 12-byte stride meant every entry scribbled 52
         * bytes over the following four entries, which happened to work because
         * the next iteration rewrote them -- but the LAST entry had nothing
         * after it, so it ran 52 bytes past the end of the driver's array into
         * guest kernel heap. On every NAND read, for the life of the run.
         *
         * Writing 0xc is behaviour-identical for every entry except that
         * overspill: bytes 0xc..0x3f were always overwritten by the next
         * iteration anyway.
         */
        cpu_physical_memory_write(s->reg_page_spare_out_addr + page_ind * 0xc,
                                  s->page_spare_buffer, 0xc);
    }
}

/*
 * Handle a flash WRITE command (csgenrc 0xa02), storing pages into the overlay.
 *
 * Unlike a read, a write descriptor is NOT a flat page array. The driver
 * (_fmssPrepareWriteSequential / _fmssPrepareWriteScatteredPages in
 * com.apple.driver.AppleS5L8720xFMSS) builds a small script of two-word
 * entries and points FMSS_CS_BUF_ADDR (0xD10) at it:
 *
 *   [2i]   command word: 0x00801000 | (1 << csShift) | plane bits,
 *                        plus 0x70000000 when this chip-select already
 *                        appeared earlier in the same script
 *   [2i+1] physical page number on that chip-select
 *   ...
 *   [2n]   0x00000000 terminator
 *
 * A real 8-entry script observed at runtime (4 chip-selects x 2 planes):
 *   00801101 00000602  70811001 00000682   <- cs0, pages 0x602 / 0x682
 *   00801102 00000602  70811002 00000682   <- cs1
 *   00801104 00000602  70811004 00000682   <- cs2
 *   00801108 00000602  70811008 00000682   <- cs3
 *   00000000                                <- terminator
 *
 * Two consequences, both different from a read:
 *  - The write path never writes FMSS_PAGES_IN_ADDR (0xD0C) or FMSS_NUM_PAGES
 *    (0xD18); those still hold the previous *read*'s values, so reg_num_pages
 *    is meaningless here. The entry count comes from the terminator.
 *  - Everything past the terminator is stale data from earlier commands that
 *    shared the same DMA buffer, and must be ignored.
 *
 * Entry i's page data is the two 2048-byte DMA source addresses at
 * pages_out[2i] and pages_out[2i+1]; its 12-byte spare record is at
 * spare_out + i*0xc (stride 0xc == the driver's 3 spare words per page).
 */

static void write_nand_pages(IPodTouchFMSSState *s)
{
    if (ipod_touch_fmss_io_failed()) {
        vm_stop(RUN_STATE_IO_ERROR);
        return;
    }
    if (!s->nand_overlay) {
        return; /* no writable overlay -> writes are discarded (original behaviour) */
    }

    uint32_t desc = s->reg_cs_buf_addr;

    /* FMSS_LEGACY: the pre-2026-07-31 reading, kept so the two decodes can be
     * compared back to back -- treat pages_in as a flat array from word 3 with
     * reg_num_pages entries, striping chip-selects from the header bitmap. */
    bool legacy = fmss_legacy_on();
    int legacy_np = s->reg_num_pages;
    uint32_t legacy_hdr = 0;
    if (legacy) {
        if (!fmss_write_dma_read(s->reg_pages_in_addr, &legacy_hdr, 4)) return;
        legacy_hdr = find_bit_index(legacy_hdr & 0xff);
        desc = s->reg_pages_in_addr;
    }

    bool terminated = false;
    for (int i = 0; i < FMSS_MAX_WRITE_ENTRIES; i++) {
        uint32_t cmd = 0, page_nr = 0;

        if (legacy) {
            if (i >= legacy_np) {
                terminated = true;
                break;
            }
            if (!fmss_write_dma_read((uint64_t)desc + (3 + i) * 4, &page_nr, 4)) return;
            cmd = 1u << ((legacy_hdr + i) & 3);
        } else {
            if (!fmss_write_dma_read((uint64_t)desc + (2 * i) * 4, &cmd, 4)) return;
            if (cmd == 0) {
                terminated = true;
                break; /* end-of-script terminator */
            }
            if (!fmss_write_dma_read((uint64_t)desc + (2 * i + 1) * 4, &page_nr, 4)) return;
        }

        uint32_t cs = find_bit_index(cmd & 0xff);
        /* A malformed entry must not acknowledge an incomplete file write. */
        unsigned select = cmd & 0xff;
        if (!select || (select & (select - 1)) || cs > 3 ||
            page_nr >= 4096 * NAND_PAGES_PER_BLOCK) {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "[fmss] bad chip-select in command word 0x%08x "
                          "(entry %d); write stopped\n", cmd, i);
            fmss_io_error("guest write chip-select", EINVAL);
            return;
        }

        /* gather the 4096-byte page from the two 2048-byte DMA source halves */
        int half = NAND_BYTES_PER_PAGE / 2;
        uint32_t src0 = 0, src1 = 0;
        if (!fmss_write_dma_read((uint64_t)s->reg_pages_out_addr + (2*i) * 4, &src0, 4) ||
            !fmss_write_dma_read((uint64_t)s->reg_pages_out_addr + (2*i + 1) * 4, &src1, 4)) return;
        memset(s->page_buffer, 0, NAND_BYTES_PER_PAGE);
        if (src0 && !fmss_write_dma_read(src0, s->page_buffer, half)) return;
        if (src1 && !fmss_write_dma_read(src1, s->page_buffer + half, half)) return;

        /* gather the 12-byte spare record, zero-padded to the on-disk 64 bytes */
        memset(s->page_spare_buffer, 0, NAND_BYTES_PER_SPARE);
        if (!fmss_write_dma_read((uint64_t)s->reg_page_spare_out_addr + i * 0xc,
                                s->page_spare_buffer, 0xc)) return;

        if (fmss_dump_on()) {
            static int nd = 0;
            if (nd++ < 400) {
                uint32_t *sp = (uint32_t *)s->page_spare_buffer;
                uint32_t d0 = 0, d1 = 0;
                memcpy(&d0, s->page_buffer, 4);
                memcpy(&d1, s->page_buffer + half, 4);
                printf("WE i=%2d cmd=%08x cs=%u page=%6u src=%08x/%08x data=%08x/%08x spare=%08x %08x %08x\n",
                       i, cmd, cs, page_nr, src0, src1, d0, d1, sp[0], sp[1], sp[2]);
                fflush(stdout);
            }
        }

        /* Before the relocation below, which can drop the page entirely: what
         * matters here is that the guest wrote these bytes, not where they end
         * up living. */
        fmss_sniff_icon_state(s->page_buffer);

        /*
         * The page reads back at the address it was programmed to for the rest
         * of this session, with the spare the guest wrote -- the FTL validates
         * the logical number it finds there, so it has to be the guest's own
         * bytes and not the synthetic "clean" spare substituted below.
         */
        uint32_t physical_cs = cs, physical_page = page_nr;
        uint8_t physical_spare[NAND_BYTES_PER_SPARE];
        memcpy(physical_spare, s->page_spare_buffer, sizeof(physical_spare));

        /*
         * Undo the FTL's relocation -- see fmss_generated_layout(). The
         * physical page the driver picked is freshly allocated as far as it is
         * concerned, but in the generated image it is still holding somebody
         * else's file data. Store the page where its logical block actually
         * lives, and leave the synthetic clean spare in place so the image
         * stays uniformly "generated".
         *
         * This is what makes the *persisted* image correct across a reboot;
         * fmss_remember_physical below makes it correct before one.
         */
        if (!fmss_physical()) {
            uint32_t logical = ldl_le_p(s->page_spare_buffer);
            uint32_t rcs, rpage;
            if (!fmss_generated_layout(s, logical, &rcs, &rpage)) {
                if (fmss_rtrace()) {
                    printf("SKIP logical=%u (cs=%u page=%u)\n", logical, cs, page_nr);
                    fflush(stdout);
                }
                /* FTL bookkeeping is intentionally session-only. */
                fmss_remember_physical(s, cs, page_nr, s->page_buffer,
                                       physical_spare);
                continue;
            }
            if (fmss_rtrace()) {
                printf("KEEP logical=%u -> cs=%u page=%u\n", logical, rcs, rpage);
                fflush(stdout);
            }
            cs = rcs;
            page_nr = rpage;
            memset(s->page_spare_buffer, 0, NAND_BYTES_PER_SPARE);
            ((uint32_t *)s->page_spare_buffer)[2] = 0x00FF00FF;
        }

        if (!fmss_store_page(s, cs, page_nr, s->page_buffer,
                             s->page_spare_buffer)) {
            return;
        }
        fmss_remember_physical(s, physical_cs, physical_page, s->page_buffer,
                               physical_spare);
    }
    if (!terminated) {
        fmss_io_error("unterminated guest write script", EINVAL);
    }
}

static uint64_t ipod_touch_fmss_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchFMSSState *s = (IPodTouchFMSSState *)opaque;
    switch(addr)
    {
        case FMSS__CS_BUF_RST_OK:
            return 0x1;
        case FMSS__CS_IRQ:
            return s->reg_cs_irq_bit;
        case FMSS__CS_IRQMASK:
            return 0x1;
        case FMSS__FMCTRL1:
            return (0x1 << 30);
        case 0xD00:
            return 42;
        case 0x00000C30:
            return 0x1;
        default:
            qemu_log_mask(LOG_UNIMP, "%s: read invalid location 0x%08x.\n",
                          __func__, (unsigned)addr);
            break;
    }
    return 0;
}

static void ipod_touch_fmss_write(void *opaque, hwaddr addr, uint64_t val, unsigned size)
{
    IPodTouchFMSSState *s = (IPodTouchFMSSState *)opaque;

    switch(addr) {
        case 0xC00:
            if(val == 0x0000ffb5) { s->reg_cs_irq_bit = 1; } // TODO ugly and hard-coded
            if(val == 0xfff5) { s->reg_cs_irq_bit = 1; qemu_set_irq(s->irq, 1); }
            break;
        case FMSS__CS_IRQ:
            s->reg_cs_irq_bit = 0;
            qemu_set_irq(s->irq, 0);
            break;
        case FMSS_CINFO_TARGET_ADDR:
            s->reg_cinfo_target_addr = val;
            write_chip_info(s);
            break;
        case FMSS_PAGES_IN_ADDR:
            s->reg_pages_in_addr = val;
            break;
        case FMSS_CS_BUF_ADDR:
            s->reg_cs_buf_addr = val;
            break;
        case FMSS_NUM_PAGES:
            /*
             * Bound to the device's own page count -- a command cannot
             * legitimately reference more pages than the NAND physically has,
             * so this can NEVER truncate a real read (kernel/ramdisk loads run
             * to thousands of pages), while still stopping a hostile
             * 0xFFFFFFFF from spinning the read/write loop for billions of
             * iterations under the BQL. A 512-page limit truncates legitimate
             * boot reads and bulk writes alike.
             */
            s->reg_num_pages = IT_SIZE("fmss", val,
                (uint64_t)fmss_total_blocks(s) * NAND_PAGES_PER_BLOCK);
            break;
        case FMSS_PAGE_SPARE_OUT_ADDR:
            s->reg_page_spare_out_addr = val;
            break;
        case FMSS_PAGES_OUT_ADDR:
            s->reg_pages_out_addr = val;
            break;
        case FMSS_CSGENRC:
            s->reg_csgenrc = val;
            break;
        case 0xD38:
            if (fmss_trace_on() && s->reg_csgenrc != 0xa01 && s->reg_csgenrc != 0xa02) {
                printf("FMSS_OP csgenrc=%08x d0c=%08x d10=%08x d18=%08x\n",
                       s->reg_csgenrc, s->reg_pages_in_addr,
                       s->reg_cs_buf_addr, s->reg_num_pages);
                fflush(stdout);
            }
            if(s->reg_csgenrc == 0xa01) { read_nand_pages(s); }
            else if(s->reg_csgenrc == 0xa02) { write_nand_pages(s); }
            else {
                /* Erase and the other opcodes are not modelled. Confirmed by
                 * instrumentation that none of them fires during boot or an
                 * app install, so erase is not what blocks persistence. */
            }
            break;
        default:
            if (fmss_trace_on()) {
                static uint8_t seen[0x1000];
                if (addr < 0x1000 && !seen[addr]) {
                    seen[addr] = 1;
                    printf("FMSS_W %04x = %08x (first)\n", (unsigned)addr, (unsigned)val);
                    fflush(stdout);
                }
            }
            break;
    }
}

static const MemoryRegionOps fmss_ops = {
    .read = ipod_touch_fmss_read,
    .write = ipod_touch_fmss_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_fmss_realize(DeviceState *dev, Error **errp)
{
    fmss_try_map_packed(IPOD_TOUCH_FMSS(dev));
}

static void ipod_touch_fmss_init(Object *obj)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);
    DeviceState *dev = DEVICE(sbd);
    IPodTouchFMSSState *s = IPOD_TOUCH_FMSS(dev);

    memory_region_init_io(&s->iomem, obj, &fmss_ops, s, "fmss", 0xF00);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    s->page_buffer = (uint8_t *)g_malloc(NAND_BYTES_PER_PAGE);
    s->page_spare_buffer = (uint8_t *)g_malloc(NAND_BYTES_PER_SPARE);
}

static void ipod_touch_fmss_finalize(Object *obj)
{
    IPodTouchFMSSState *s = IPOD_TOUCH_FMSS(obj);

    g_free(s->page_buffer);
    g_free(s->page_spare_buffer);
    if (s->phys_pages) {
        g_hash_table_destroy(s->phys_pages);
    }
    if (s->erased_blocks) {
        g_hash_table_destroy(s->erased_blocks);
    }
}

/* Generated images persist writes at their logical home, while phys_pages
 * serves the running FTL's temporary physical mapping. A new boot rebuilds
 * the FTL from that generated layout, so retaining the previous mapping can
 * shadow its HFS header with unrelated data. Physical-image mode does not
 * relocate writes and must retain actual programmed pages across reset.
 */
static void ipod_touch_fmss_reset(DeviceState *dev)
{
    IPodTouchFMSSState *s = IPOD_TOUCH_FMSS(dev);

    if (!fmss_physical() && s->phys_pages) {
        g_hash_table_remove_all(s->phys_pages);
    }
    s->reg_cs_irq_bit = 0;
    s->reg_cinfo_target_addr = 0;
    s->reg_pages_in_addr = 0;
    s->reg_cs_buf_addr = 0;
    s->reg_num_pages = 0;
    s->reg_page_spare_out_addr = 0;
    s->reg_pages_out_addr = 0;
    s->reg_csgenrc = 0;
    memset(s->page_buffer, 0, NAND_BYTES_PER_PAGE);
    memset(s->page_spare_buffer, 0, NAND_BYTES_PER_SPARE);
    iboot_bt_patched = false;
    if (s->irq) {
        qemu_irq_lower(s->irq);
    }
}

/*
 * KNOWN GAP, deliberate: phys_pages and erased_blocks are GHashTables holding
 * this session's programmed pages, and they are not migrated. The persisted
 * side of a write is in the NAND overlay directory, which the destination
 * opens for itself, so file content survives; what does not survive is the
 * physical-page memory that makes a page the FTL just relocated read back at
 * the address it was programmed to. In practice that mapping only has to hold
 * until the FTL is rebuilt, which a restore does not disturb.
 *
 * page_buffer/page_spare_buffer are the DMA staging area for one transfer;
 * they are only meaningful between the register write that starts a transfer
 * and its completion, which cannot straddle a snapshot.
 */
static const VMStateDescription vmstate_ipod_touch_fmss = {
    .name = "ipod_touch_fmss",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(reg_cs_irq_bit, IPodTouchFMSSState),
        VMSTATE_UINT32(reg_cinfo_target_addr, IPodTouchFMSSState),
        VMSTATE_UINT32(reg_pages_in_addr, IPodTouchFMSSState),
        VMSTATE_UINT32(reg_cs_buf_addr, IPodTouchFMSSState),
        VMSTATE_UINT32(reg_num_pages, IPodTouchFMSSState),
        VMSTATE_UINT32(reg_page_spare_out_addr, IPodTouchFMSSState),
        VMSTATE_UINT32(reg_pages_out_addr, IPodTouchFMSSState),
        VMSTATE_UINT32(reg_csgenrc, IPodTouchFMSSState),
        VMSTATE_UINT32(total_blocks, IPodTouchFMSSState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_fmss_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = ipod_touch_fmss_realize;
    device_class_set_legacy_reset(dc, ipod_touch_fmss_reset);
    dc->vmsd = &vmstate_ipod_touch_fmss;
}

static const TypeInfo ipod_touch_fmss_info = {
    .name          = TYPE_IPOD_TOUCH_FMSS,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchFMSSState),
    .instance_init = ipod_touch_fmss_init,
    .instance_finalize = ipod_touch_fmss_finalize,
    .class_init    = ipod_touch_fmss_class_init,
};

static void ipod_touch_machine_types(void)
{
    type_register_static(&ipod_touch_fmss_info);
}

type_init(ipod_touch_machine_types)
