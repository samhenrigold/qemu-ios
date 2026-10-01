#!/usr/bin/env python3
"""Validate real FMSS read leaves with filesystem failures and sanitizers.

The controller stores a 64-byte metadata projection, not complete raw NAND
OOB. Corrupt/short/open-error checks preserve the current fallback policy;
they do not qualify that policy as correct physical NAND behavior.
"""
import argparse
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--source", type=Path,
                    default=ROOT / "hw/arm/ipod_touch_fmss.c",
                    help="alternate actual source for a negative baseline run")
source = parser.parse_args().source.read_text()
functions = []
enum = re.search(r"typedef enum FMSSPackedRead.*?} FMSSPackedRead;",
                 source, re.S)
if enum:
    functions.append(enum.group())
for name in (
    "fmss_block_key", "fmss_key_compare", "fmss_remember_erased",
    "fmss_block_marker_path", "fmss_block_is_erased", "fmss_packed_page",
    "fmss_blank_page", "fmss_load_page_inner",
):
    # The pre-fix baseline has no shared blank initializer.
    if name == "fmss_blank_page" and "static void fmss_blank_page(" not in source:
        continue
    pattern = (r"^(?:static )?[A-Za-z_][^\n]*\b" + name +
               r"\([^)]*\)[^;{]*\{.*?^}")
    match = re.search(pattern, source, re.M | re.S)
    assert match, name
    functions.append(match.group())

code = r'''
#include <glib.h>
#include <glib/gstdio.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#define NAND_BYTES_PER_PAGE 4096
#define NAND_BYTES_PER_SPARE 64
#define NAND_PAGES_PER_BLOCK 128
#define LOG_GUEST_ERROR 1
#define qemu_log_mask(...) ((void)0)
static bool physical, overlay_claim;
static bool
fmss_physical(void)
{
    return physical;
}
static bool
fmss_erase_on(void)
{
    return true;
}
static bool
fmss_usedspare(void)
{
    return false;
}
static bool
fmss_basespare(void)
{
    return false;
}
static bool
fmss_rtrace(void)
{
    return false;
}
static uint32_t ldl_le_p(const void *p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return GUINT32_FROM_LE(v);
}
static struct {
    unsigned recall, overlay, base, blank;
} fmss_stats;
typedef struct {
    char *nand_path, *nand_overlay;
    GTree *erased_blocks;
    const uint8_t *packed, *packed_records;
    const uint32_t *packed_index;
    unsigned packed_num_cs, packed_pages_per_cs;
    size_t packed_record_count;
} IPodTouchFMSSState;
static bool
fmss_recall_physical(IPodTouchFMSSState *s, unsigned cs, unsigned p, uint8_t *d, uint8_t *sp)
{
    return false;
}
static bool fmss_overlay_has(IPodTouchFMSSState *s, unsigned cs, unsigned p)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/cs%u/%u.page", s->nand_overlay, cs, p);
    return overlay_claim || g_file_test(path, G_FILE_TEST_EXISTS);
}
''' + '\n'.join(functions) + r'''
static void
encoding(IPodTouchFMSSState *s, unsigned p, bool erased)
{
    uint8_t d[4096], sp[64];
    memset(d, 0xa3, sizeof(d));
    memset(sp, 0xa3, sizeof(sp));
    fmss_load_page_inner(s, 0, p, d, sp);
    for (unsigned i = 0; i < 4096; i++)
        assert(d[i] == (fmss_physical() && erased ? 255 : 0));
    for (unsigned i = 0; i < 64; i++)
        assert(sp[i] == (fmss_physical() && erased ? 255 : ((i == 8 || i == 10) ? 255 : 0)));
}
static void blank(IPodTouchFMSSState *s, unsigned p)
{
    encoding(s, p, true);
}
static void
present(IPodTouchFMSSState *s, unsigned p, uint8_t value)
{
    uint8_t d[4096], sp[64];
    fmss_load_page_inner(s, 0, p, d, sp);
    for (unsigned i = 0; i < 4096; i++)
        assert(d[i] == value);
    for (unsigned i = 0; i < 64; i++)
        assert(sp[i] == value);
}
int
main(int argc, char **argv)
{
    assert(argc == 2);
    char *base = g_build_filename(argv[1], "base", NULL), *over = g_build_filename(argv[1], "overlay", NULL);
    char *cs = g_build_filename(base, "cs0", NULL), *ocs = g_build_filename(over, "cs0", NULL);
    assert(g_mkdir_with_parents(cs, 0755) == 0 && g_mkdir_with_parents(ocs, 0755) == 0);
    IPodTouchFMSSState s = {.nand_path = base,.nand_overlay = over,.erased_blocks = g_tree_new_full(fmss_key_compare, NULL, NULL, g_free)};
    for (physical = false;; physical = true) {
        blank(&s, 129); /*legitimate absent directory page */
        /*
         *Short records retain their existing zero-tail policy in both
         *modes.
         */
        char *shortpath = g_build_filename(cs, "6.page", NULL);
        uint8_t partial[20], pd[4096], ps[64];
        memset(partial, 0x65, sizeof(partial));
        assert(g_file_set_contents(shortpath, (char *)partial, sizeof(partial), NULL));
        fmss_load_page_inner(&s, 0, 6, pd, ps);
        for (unsigned i = 0; i < 4096; i++)
            assert(pd[i] == (i < 20 ? 0x65 : 0));
        for (unsigned i = 0; i < 64; i++)
            assert(ps[i] == 0);
        unlink(shortpath);
        g_free(shortpath);
        uint8_t record[4160];
        memset(record, 0xa3, sizeof(record));
        char *p = g_build_filename(cs, "5.page", NULL);
        assert(g_file_set_contents(p, (char *)record, sizeof(record), NULL));
        present(&s, 5, 0xa3);
        char *marker = g_build_filename(ocs, "blk0.erased", NULL);
        assert(g_file_set_contents(marker, "", 0, NULL));
        blank(&s, 5);
        /*
         *The persisted marker path, not just the memory cache, must
         *qualify.
         */
        g_tree_remove_all(s.erased_blocks);
        blank(&s, 5);
        char *op = g_build_filename(ocs, "5.page", NULL);
        memset(record, 0x79, sizeof(record));
        assert(g_file_set_contents(op, (char *)record, sizeof(record), NULL));
        present(&s, 5, 0x79);
        unlink(op);
        unlink(marker);
        g_tree_remove_all(s.erased_blocks);
        present(&s, 5, 0xa3);
        uint32_t index[256] = {0};
        index[129] = GUINT32_TO_LE(1);
        s.packed = (void *)index;
        s.packed_index = index;
        s.packed_num_cs = 1;
        s.packed_pages_per_cs = 256;
        s.packed_record_count = 1;
        s.packed_records = record;
        blank(&s, 130);
        present(&s, 129, 0x79);
        /*Bad slot and out-of-index address preserve the existing fallback. */
        index[130] = GUINT32_TO_LE(2);
        encoding(&s, 130, false);
        encoding(&s, 256, false);
        s.packed_num_cs = 0;
        encoding(&s, 129, false);
        s.packed_num_cs = 1;
        s.packed = NULL;
        /*
         *Real ENOTDIR on base open: preserve fallback instead of claiming
         *FF.
         */
        char *badroot = g_build_filename(argv[1], "not-a-directory", NULL);
        assert(g_file_set_contents(badroot, "x", 1, NULL));
        s.nand_path = badroot;
        encoding(&s, 129, false);
        s.nand_path = base;
        /*
         *Indexed overlay failure still permits old base fallback. If base
         *is also absent, its ENOENT must not hide the preceding overlay
         *error.
         */
        s.nand_overlay = badroot;
        overlay_claim = true;
        present(&s, 5, 0xa3);
        encoding(&s, 129, false);
        s.packed = (void *)index;
        s.packed_index = index;
        s.packed_num_cs = 1;
        index[130] = 0;
        encoding(&s, 130, false);
        s.packed = NULL;
        /*
         *A remembered erase does not turn an inaccessible programmed
         *overlay into FF either; preserve the old permissive fallback, not
         *endorse it.
         */
        fmss_remember_erased(&s, 0, 1);
        encoding(&s, 129, false);
        g_tree_remove_all(s.erased_blocks);
        overlay_claim = false;
        s.nand_overlay = over;
        unlink(badroot);
        g_free(badroot);
        unlink(p);
        g_free(p);
        g_free(op);
        g_free(marker);
        if (physical)
            break;
    }
    g_tree_destroy(s.erased_blocks);
    g_free(base);
    g_free(over);
    g_free(cs);
    g_free(ocs);
    puts("PASS physical FF and generated legacy blanks: directory, packed, durable erased-marker precedence, exact present records and preserved corrupt/short/non-ENOENT fallback");
}
'''

with tempfile.TemporaryDirectory(prefix="fmss-blank-") as temporary:
    directory = Path(temporary)
    c_file = directory / "test.c"
    binary = directory / "test"
    c_file.write_text(code)
    flags = shlex.split(subprocess.check_output(
        ["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    subprocess.run([
        "cc", "-std=c11", "-Wall", "-Werror", "-fsanitize=address,undefined",
        "-fno-sanitize-recover=all", str(c_file), *flags, "-o", str(binary),
    ], check=True)
    subprocess.run([str(binary), str(directory)], check=True)
