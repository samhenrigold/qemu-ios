#!/usr/bin/env python3
"""it_iboot_find_miu_epoch reads the boot security epoch off every iPad 1 iBoot, 817 .. 1219.

Host only. iBoot's miu_init demands POWER_ID[31:24] == epoch(), the CHIPID fuse field floored at the build's
epoch; on hardware LLB writes that byte, and the machine's iboot= path (which skips LLB) writes what LLB would.
Images come from the decrypt caches imgtools/ipad1_device.py leaves (~/Developer/qemu-ios-files/ipad1/repro/cache/
<ipsw sha1>/iBoot.bin) or the iOS 5 spike's (~/Developer/qemu-ios-files/ios5-spike/dec-<build>/); missing builds
are skipped. Pinned: the model's fuse field (1, docs/ipad1/hw1-probes.log) gives 1 for 3.2/4.2 and 2 from 4.3;
a fuse above the floor wins; two copies of the check refuse.
"""
import subprocess, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
FILES = Path('~/Developer/qemu-ios-files').expanduser()
# build: (image candidates, iBoot version, epoch for fuse 0, 1, 3)
BUILDS = {
    '7B500': (['ipad1/repro/cache/68b613f78581d36eab96aa5a007001dff142baa3', 'ipad1/7B500/dec'], 'iBoot-817.29', (1, 1, 3)),
    '8C148': (['ipad1/repro/cache/8717b3bedc925b587566442ad375aa65d857e79a'], 'iBoot-931.71.16', (1, 1, 3)),
    '8L1':   (['ios5-spike/dec-8L1'], 'iBoot-1072.61', (2, 2, 3)),
    '9A405': (['ios5-spike/dec-9A405'], 'iBoot-1219.43.32', (2, 2, 3)),
    '9B206': (['ipad1/repro/cache/ad9b607439250f2337fe132890dadc4c487beca8', 'ios5-spike/dec-9B206'], 'iBoot-1219.62.15', (2, 2, 3)),
}
HARNESS = r'''
#include "qemu/osdep.h"
#include "hw/arm/it_iboot.h"
#include <stdio.h>
int main(int argc, char **argv) {
    static uint8_t img[0x200000];
    FILE *f = fopen(argv[1], "rb"); size_t n = fread(img, 1, 0x100000, f); fclose(f);
    printf("%u %u %u", it_iboot_find_miu_epoch(img, n, 0), it_iboot_find_miu_epoch(img, n, 1), it_iboot_find_miu_epoch(img, n, 3));
    memcpy(img + n, img, n);   /* the check twice: ambiguous */
    printf(" %u\n", it_iboot_find_miu_epoch(img, 2 * n, 1));
    return 0;
}
'''

with tempfile.TemporaryDirectory() as temp:
    p = Path(temp)
    (p / 'qemu').mkdir()
    (p / 'qemu/osdep.h').write_text('#pragma once\n#include <stdbool.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n')
    (p / 'check.c').write_text(HARNESS)
    subprocess.run(['clang', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DIT_IBOOT_HOST_TEST',
                    '-I' + str(p), '-I' + str(ROOT / 'include'), str(p / 'check.c'), str(ROOT / 'hw/arm/it_iboot.c'),
                    '-o', str(p / 'check')], check=True)
    seen = 0
    for build, (dirs, version, want) in BUILDS.items():
        image = next((FILES / d / 'iBoot.bin' for d in dirs if (FILES / d / 'iBoot.bin').exists()), None)
        if image is None:
            print('SKIP %s: no decrypted iBoot' % build)
            continue
        assert version.encode() in image.read_bytes(), (build, version)
        got = [int(x) for x in subprocess.run([str(p / 'check'), str(image)], check=True, capture_output=True,
                                               text=True).stdout.split()]
        assert got == [*want, 0], (build, got, want)
        print('%-6s %-17s epoch %d (fuse 0: %d, fuse 3: %d)' % (build, version, want[1], want[0], want[2]))
        seen += 1
    assert seen, 'no iBoot image found for any build'
    print('PASS: it_iboot_find_miu_epoch on %d/%d iPad 1 iBoots' % (seen, len(BUILDS)))
