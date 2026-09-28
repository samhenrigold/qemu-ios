#!/usr/bin/env python3
"""hw/arm/guest-package.c's core on the host under ASan/UBSan, with a guest-memory fixture."""
from pathlib import Path
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
check = r'''
#include "qemu/osdep.h"
#include "hw/arm/guest-package.h"
static uint8_t memory[4096];
static bool fail;
static int copy(void *unused, uint32_t addr, uint8_t *data, size_t len, bool write) {
    if (fail || addr > sizeof(memory) || len > sizeof(memory) - addr) return -1;
    if (write) memcpy(memory + addr, data, len); else memcpy(data, memory + addr, len);
    return 0;
}
static int64_t op(GuestPackage *p, unsigned o, uint64_t token, uint32_t off, uint32_t len) {
    return guest_pkg_op(p, o, token, 0, off, len, copy, NULL);
}
static void put(const char *dir, const char *name, const char *data, size_t len) {
    char *path = g_build_filename(dir, name, NULL);
    char *parent = g_path_get_dirname(path);
    g_mkdir_with_parents(parent, 0755);
    assert(g_file_set_contents(path, data, len, NULL));
    g_free(path); g_free(parent);
}
int main(int argc, char **argv) {
    const char *dir = argv[1];
    GuestPackage p = { .file_idx = -1 };
    int64_t serial; int32_t result;
    assert(argc == 2);

    /* no directory, then a directory without an offer: a silent host */
    assert(op(&p, 0x170, 0, 0, 0) == -1 && op(&p, 0x170, 0, 0, 16) == -1);
    assert(op(&p, 0x171, 0, 0, 0) == -1);
    guest_pkg_set_dir(&p, dir);
    assert(op(&p, 0x170, 0, 0, 0) == -1);

    static char big[3000];
    for (unsigned i = 0; i < sizeof(big); i++) big[i] = i * 7;
    put(dir, "bin/tool", big, sizeof(big));
    put(dir, "jobs/x.plist", "<plist/>", 8);
    put(dir, "secret", "no", 2);
    const char *offer =
        "ltpkg 1\nserial 14 1.4.0\n"
        "file 0 bin/tool 755 3000 0\n"
        "job  1 jobs/x.plist 644 8 0\n"
        "file 2 ../secret 644 2 0\n"
        "file 3 /etc/passwd 644 2 0\n"
        "file 0 secret 644 2 0\n"          /* a duplicate index keeps the first */
        "file 64 secret 644 2 0\n"
        "hook 4 missing 755 1 0 /System/x respring\n";
    put(dir, "offer", offer, strlen(offer));
    int64_t total = op(&p, 0x170, 0, 0, 0);
    assert(total == (int64_t)strlen(offer));
    char got[4096] = "";
    for (int64_t off = 0; off < total;) {
        int64_t n = op(&p, 0x170, 0, off, 100);
        assert(n > 0 && n <= 100);
        memcpy(got + off, memory, n); off += n;
    }
    assert(!memcmp(got, offer, total) && op(&p, 0x170, 0, total, 100) == 0);
    assert(op(&p, 0x170, 0, 0, 1025) == -1);
    fail = true; assert(op(&p, 0x170, 0, 0, 10) == -1); fail = false;

    /* payloads by index only, confined to the directory */
    assert(op(&p, 0x171, 0, 0, 0) == 3000);
    uint8_t whole[3000];
    for (int off = 0; off < 3000;) {
        int64_t n = op(&p, 0x171, 0, off, 1024);
        assert(n > 0); memcpy(whole + off, memory, n); off += n;
    }
    assert(!memcmp(whole, big, 3000) && op(&p, 0x171, 0, 3000, 1024) == 0);
    assert(op(&p, 0x171, 0, 0, 1025) == -1);
    assert(op(&p, 0x171, 1, 0, 0) == 8 && op(&p, 0x171, 1, 0, 8) == 8 && !memcmp(memory, "<plist/>", 8));
    assert(op(&p, 0x171, 2, 0, 0) == -1 && op(&p, 0x171, 3, 0, 0) == -1);
    assert(op(&p, 0x171, 4, 0, 0) == -1 && op(&p, 0x171, 5, 0, 0) == -1);
    assert(op(&p, 0x171, 64, 0, 0) == -1 && op(&p, 0x171, UINT64_MAX, 0, 0) == -1);
    fail = true; assert(op(&p, 0x171, 0, 0, 10) == -1); fail = false;

    /* too big to serve */
    char *path = g_build_filename(dir, "bin/tool", NULL);
    assert(!truncate(path, GUEST_PKG_FILE_MAX + 1));
    assert(op(&p, 0x171, 1, 0, 0) == 8 && op(&p, 0x171, 0, 0, 0) == -1);
    g_free(path);

    /* the offer is re-read only when the guest opens it (length 0) */
    put(dir, "offer", "ltpkg 1\nserial 0\n", 17);
    assert(op(&p, 0x170, 0, 0, 7) == 7 && !memcmp(memory, "ltpkg 1", 7));
    assert(op(&p, 0x170, 0, total - 10, 100) == 10);
    assert(op(&p, 0x170, 0, 0, 0) == 17 && op(&p, 0x171, 1, 0, 0) == -1);
    guest_pkg_set_dir(&p, "");
    assert(op(&p, 0x170, 0, 0, 0) == -1);

    /* reports and the GL hello, readable from the helper's side */
    assert(!guest_pkg_last_report(&serial, &result));
    assert(guest_pkg_gles_protocol(&serial) == 0 && serial == 0);
    memcpy(memory, "prev 13\001good", 12);
    assert(op(&p, 0x172, 14, (uint32_t)-5, 12) == 0);
    assert(guest_pkg_last_report(&serial, &result) && serial == 14 && result == -5);
    char *status = guest_pkg_status();
    assert(strstr(status, "report 14 -5 (1 this boot) prev 13?good") && strstr(status, "no gles-hello"));
    g_free(status);
    assert(op(&p, 0x172, 1, 0, 1025) == -1);
    fail = true; assert(op(&p, 0x172, 1, 0, 3) == -1); fail = false;
    assert(guest_pkg_last_report(&serial, &result) && serial == 14);
    assert(op(&p, 0x142, 15, 1, 0) == GUEST_GLES_PROTO);
    assert(guest_pkg_gles_protocol(&serial) == 1 && serial == 15);
    assert(op(&p, 0x999, 0, 0, 0) == -1);
    guest_pkg_reset(&p);
    assert(!guest_pkg_last_report(&serial, &result) && guest_pkg_gles_protocol(NULL) == 0);
    guest_pkg_set_dir(&p, NULL);
    puts("PASS: silent host, offer windows and snapshots, indexed payloads confined to the directory, "
         "bounds, memory faults, reports, GL hello, reset");
    return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    tmp = Path(d)
    (tmp / "qemu").mkdir()
    (tmp / "qemu/osdep.h").write_text('''#pragma once
#include <assert.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <glib.h>
''')
    (tmp / "check.c").write_text(check)
    (tmp / "offer").mkdir()
    flags = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "glib-2.0"], text=True))
    subprocess.run(["clang", "-g", "-Wall", "-Werror", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                    "-DGUEST_PKG_CORE_ONLY", "-I" + d, "-I" + str(root / "include"),
                    str(root / "hw/arm/guest-package.c"), str(tmp / "check.c"), "-o", str(tmp / "check"), *flags],
                   check=True)
    subprocess.run([str(tmp / "check"), str(tmp / "offer")], check=True)
