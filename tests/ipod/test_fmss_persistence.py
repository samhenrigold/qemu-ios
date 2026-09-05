#!/usr/bin/env python3
"""Compile the real FMSS write path with host-I/O fault injection; no guest needed."""
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
source = (ROOT / "hw/arm/ipod_touch_fmss.c").read_text()
functions = []
for name in ("find_bit_index", "fmss_block_key", "fmss_block_marker_path",
             "fmss_block_is_erased", "ipod_touch_fmss_io_failed", "fmss_io_error", "fmss_erase_block",
             "fmss_remember_physical", "fmss_store_page", "write_nand_pages"):
    match = re.search(r"^(?:static )?[^\n]*\b" + name + r"\([^)]*\)[^{]*\{.*?^}", source, re.M | re.S)
    assert match, name
    functions.append(match.group())

harness = r'''
#include <glib.h>
#include <glib/gstdio.h>
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define NAND_BYTES_PER_PAGE 4096
#define NAND_BYTES_PER_SPARE 64
#define NAND_PAGES_PER_BLOCK 128
#define FMSS_MAX_WRITE_ENTRIES 512
#define RUN_STATE_IO_ERROR 1
#define LOG_GUEST_ERROR 1
#define qemu_log_mask(...) ((void)0)
#define error_report(...) ((void)0)
#define qatomic_read(p) (*(p))
#define qatomic_set(p, value) (*(p) = (value))
static bool fmss_io_failed;
static int stops, erase_enabled, physical_enabled, fault_at, calls;
static const char *fault;
static struct { uint64_t shadowed; } fmss_stats;
typedef struct {
    char *nand_overlay;
    GHashTable *phys_pages, *erased_blocks, *overlay_pages;
    uint32_t reg_cs_buf_addr, reg_pages_in_addr, reg_num_pages;
    uint32_t reg_pages_out_addr, reg_page_spare_out_addr;
    uint8_t page_buffer[4096], page_spare_buffer[64];
} IPodTouchFMSSState;
static uint8_t memory[10000];
static void cpu_physical_memory_read(uint32_t addr, void *p, size_t n)
{ assert(addr + n <= sizeof(memory)); memcpy(p, memory + addr, n); }
static uint32_t ldl_le_p(const void *p)
{ uint32_t n; memcpy(&n, p, 4); return GUINT32_FROM_LE(n); }
static int vm_stop(int state) { assert(state == RUN_STATE_IO_ERROR); stops++; return 0; }
static bool fmss_erase_on(void) { return erase_enabled; }
static bool fmss_physical(void) { return physical_enabled; }
static bool fmss_legacy_on(void) { return false; }
static bool fmss_dump_on(void) { return false; }
static bool fmss_rtrace(void) { return false; }
static void fmss_sniff_icon_state(const uint8_t *data) {}
static void fmss_overlay_index(IPodTouchFMSSState *s) {}
static bool fmss_generated_layout(IPodTouchFMSSState *s, uint32_t logical,
                                  uint32_t *cs, uint32_t *page)
{ *cs = 1; *page = 2; return logical == 7; }
static bool fail(const char *op)
{ if (fault && !strcmp(fault, op) && ++calls == fault_at) { errno = EIO; return true; } return false; }
static FILE *test_fopen(const char *p, const char *m)
{ return fail("open") ? NULL : fopen(p, m); }
static size_t test_fwrite(const void *p, size_t s, size_t n, FILE *f)
{ return fail("write") ? 0 : fwrite(p, s, n, f); }
static int test_fflush(FILE *f) { return fail("flush") ? -1 : fflush(f); }
static int test_fsync(int fd) { return fail("sync") ? -1 : fsync(fd); }
static int test_fclose(FILE *f) { int r = fclose(f); return fail("close") ? -1 : r; }
static int test_rename(const char *a, const char *b)
{ return fail("rename") ? -1 : rename(a, b); }
static int test_remove(const char *p) { return fail("remove") ? -1 : remove(p); }
static int test_mkdir(const char *p, int mode)
{ return fail("mkdir") ? -1 : g_mkdir_with_parents(p, mode); }
#define fopen test_fopen
#define fwrite test_fwrite
#define fflush test_fflush
#define fsync test_fsync
#define fclose test_fclose
#define rename test_rename
#define remove test_remove
#define g_mkdir_with_parents test_mkdir
'''
harness += "\n".join(functions)
harness += r'''
#undef fopen
#undef fwrite
#undef fclose
static void run_case(const char *op, int at, bool erase, bool physical)
{
    static int count;
    char dir[64], old_path[128];
    snprintf(dir, sizeof(dir), "case%d", count++);
    assert(mkdir(dir, 0700) == 0);
    IPodTouchFMSSState s = { .nand_overlay = dir, .reg_cs_buf_addr = 16,
        .reg_pages_out_addr = 64, .reg_page_spare_out_addr = 80 };
    s.overlay_pages = g_hash_table_new(g_direct_hash, g_direct_equal);
    snprintf(old_path, sizeof(old_path), "%s/cs%d", dir, physical ? 0 : 1);
    assert(g_mkdir_with_parents(old_path, 0700) == 0);
    snprintf(old_path, sizeof(old_path), "%s/cs%d/%d.page", dir,
             physical ? 0 : 1, physical ? 128 : 2);
    FILE *f = fopen(old_path, "wb");
    assert(f && fwrite("old", 1, 3, f) == 3 && fclose(f) == 0);
    memset(memory, 0, sizeof(memory));
    uint32_t words[] = { 1, 128, 0 }, addresses[] = { 1024, 3072 }, logical = 7;
    memcpy(memory + 16, words, sizeof(words));
    memcpy(memory + 64, addresses, sizeof(addresses));
    memcpy(memory + 80, &logical, 4);
    memset(memory + 1024, 0xa5, 4096);
    fmss_io_failed = false;
    fault = op; fault_at = at; calls = stops = 0;
    erase_enabled = erase; physical_enabled = physical;
    write_nand_pages(&s);
    fault = NULL;
    if (op) {
        assert(stops == 1 && ipod_touch_fmss_io_failed() && !s.phys_pages);
        assert(g_hash_table_size(s.overlay_pages) == 0);
        if (erase && strcmp(op, "remove")) {
            char marker[128];
            snprintf(marker, sizeof(marker), "%s/cs0/blk1.erased", dir);
            assert(access(marker, F_OK) != 0);
        }
        /* A resumed session cannot silently accept the failed command. */
        write_nand_pages(&s);
        assert(stops == 2 && !s.phys_pages);
        if (!erase || strcmp(op, "rename")) {
            char old[3];
            f = fopen(old_path, "rb");
            assert(f && fread(old, 1, 3, f) == 3 && !memcmp(old, "old", 3));
            fclose(f);
        }
    } else {
        assert(!stops && !ipod_touch_fmss_io_failed());
        uint8_t *cached = g_hash_table_lookup(s.phys_pages, fmss_block_key(0, 128));
        assert(cached && cached[0] == 0xa5 && ldl_le_p(cached + 4096) == 7);
        struct stat st;
        assert(stat(old_path, &st) == 0 && st.st_size == 4160);
        assert(g_hash_table_contains(s.overlay_pages, fmss_block_key(physical ? 0 : 1,
                                                                    physical ? 128 : 2)));
    }
    if (s.phys_pages) g_hash_table_destroy(s.phys_pages);
    if (s.erased_blocks) g_hash_table_destroy(s.erased_blocks);
    g_hash_table_destroy(s.overlay_pages);
}
int main(void)
{
    const char *ops[] = { "mkdir", "open", "write", "flush", "sync", "close", "rename" };
    for (int i = 0; i < G_N_ELEMENTS(ops); i++) run_case(ops[i], 1, false, true);
    /* Stage-write failure must not erase an existing block. */
    run_case("write", 1, true, true);
    /* Marker open/flush/fsync/close fail after the staged page succeeds. */
    run_case("open", 2, true, true);
    run_case("flush", 2, true, true);
    run_case("sync", 2, true, true);
    run_case("close", 2, true, true);
    run_case("rename", 1, true, true);
    run_case("remove", 1, true, true);
    run_case(NULL, 1, false, true);
    run_case(NULL, 1, true, true);
    run_case(NULL, 1, false, false);
    run_case("rename", 1, false, false);
    puts("FMSS persistence: 18 success/fault cases passed");
}
'''
# stdbool is normally supplied by qemu/osdep.h.
harness = "#include <stdbool.h>\n" + harness
with tempfile.TemporaryDirectory(prefix="fmss-test-") as tmp:
    tmp = Path(tmp)
    (tmp / "test.c").write_text(harness)
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu11", "-Wall",
                    "-Werror", "-Wno-unused-function", str(tmp / "test.c"),
                    "-o", str(tmp / "test"), *flags], check=True)
    subprocess.run([str(tmp / "test")], cwd=tmp, check=True)
