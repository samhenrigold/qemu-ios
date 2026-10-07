#!/usr/bin/env python3
"""An unchanged frame is not republished; a changed one is (LightTouchMac #26)."""
from pathlib import Path
import re, shlex, subprocess, tempfile
root = Path(__file__).resolve().parents[2]
source = (root / 'contrib/ios-app/qemu-ios-ui.c').read_text()
match = re.search(r'^static void ios_capture\([^)]*\)\s*\{.*?^}', source, re.M | re.S)
assert match, 'ios_capture'
prelude = r'''
#include <assert.h>
#include <glib.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef int QemuMutex;
static void qemu_mutex_lock(QemuMutex *m) { assert(!*m); *m = 1; }
static void qemu_mutex_unlock(QemuMutex *m) { assert(*m); *m = 0; }
static void ios_init_frame_lock(void) {}
static void ios_report_first_lit_frame(const uint8_t *p, int w, int h) {}
typedef void (*qemu_ios_frame_cb)(void *);
#define IOS_FRAME_BUFFERS 3
static struct {
 QemuMutex frame_lock; void *buf[3]; size_t buf_size; GSList *retired;
 int published, width, height; uint64_t serial; qemu_ios_frame_cb cb; void *cb_opaque;
} ios = { .published = -1 };
typedef struct { int w, h, stride; uint8_t *data; } DisplaySurface;
static int surface_width(DisplaySurface *s) { return s->w; }
static int surface_height(DisplaySurface *s) { return s->h; }
static int surface_stride(DisplaySurface *s) { return s->stride; }
static void *surface_data(DisplaySurface *s) { return s->data; }
'''
tests = r'''
int main(void) {
 enum { W = 4, H = 3, STRIDE = 20 };   /* padded rows: only W*4 bytes count */
 uint8_t px[H * STRIDE]; memset(px, 7, sizeof px);
 DisplaySurface s = { W, H, STRIDE, px };
 ios_capture(&s); assert(ios.serial == 1);
 ios_capture(&s); assert(ios.serial == 1);            /* identical: kept */
 px[STRIDE - 1] = 9; ios_capture(&s); assert(ios.serial == 1);  /* padding only */
 px[2 * STRIDE + 3] = 8; ios_capture(&s); assert(ios.serial == 2);  /* last row */
 ios_capture(&s); assert(ios.serial == 2);
 s.w = 2; ios_capture(&s); assert(ios.serial == 3);   /* new size publishes */
 puts("PASS: unchanged frames keep their serial; changed pixels and sizes publish");
}
'''
flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True))
with tempfile.TemporaryDirectory() as tmp:
    path = Path(tmp) / 'dedupe.c'; binary = Path(tmp) / 'dedupe'
    path.write_text(prelude + match.group() + tests)
    subprocess.run(['cc', '-std=c11', '-fsanitize=address,undefined', str(path), '-o', str(binary), *flags], check=True)
    subprocess.run([str(binary)], check=True)
