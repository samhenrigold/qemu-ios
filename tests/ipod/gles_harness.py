"""Shared scaffolding for the host-GL unit tests that lift code out of gles-host.c.

PRELUDE is the file's own platform block, constants, types and GLESHost, so a
harness compiles against the real layout instead of a copy that drifts. Each
test still supplies gles_guest_rw (declared by gles.h) with its own fake guest
memory, plus whatever other stubs the lifted functions need.
"""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
src = (root / 'hw/arm/gles-host.c').read_text()


def function(name, text=src):
    """The whole top-level definition whose declaration contains `name`."""
    at = text.index(name)
    start = text.rfind('\nstatic ', 0, at) + 1
    return text[start:text.index('\n}', at) + 2]


PRELUDE = r'''
#define GL_SILENCE_DEPRECATION
#include <glib.h>
#include <assert.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef struct CPUState CPUState;
typedef uint64_t vaddr;
typedef uint64_t hwaddr;
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#include "hw/arm/guest-services/gles.h"
''' + src[src.index('#include <TargetConditionals.h>'):src.index('/* Old guest engines retain')] + r'''
static GLESHost gh_legacy;
static GLESHost *gh_current = &gh_legacy;
#define gh (*gh_current)
bool gles_guest_fault_pending(void) { return false; }
'''


def build_and_run(code, prefix):
    with tempfile.TemporaryDirectory(prefix=prefix) as tmp:
        c = Path(tmp) / 'check.c'
        exe = Path(tmp) / 'check'
        c.write_text(code)
        glib = subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True).split()
        fw = [f for name in ('OpenGL', 'VideoToolbox', 'CoreVideo', 'CoreMedia', 'CoreFoundation')
              for f in ('-framework', name)]
        subprocess.run(['clang', '-g', '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-Wno-unused-function',
                        # lifted mbxshim code is 32-bit guest code
                        '-Wno-pointer-to-int-cast', '-Wno-pointer-bool-conversion', '-I' + str(root / 'include'), str(c), '-o', str(exe), *glib, *fw],
                       check=True)
        subprocess.run([str(exe)], check=True)
