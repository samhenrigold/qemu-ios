#!/usr/bin/env python3
"""Production boot-image staging rejects bad files before any guest write."""
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

root = Path(__file__).resolve().parents[2]
source = (root / 'hw/arm/ipod_touch_2g.c').read_text()
function = re.search(r'static size_t ipod_touch_stage_boot_image\(.*?^}', source, re.M | re.S)[0]
code = r'''
#include <glib.h>
#include <assert.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
typedef uint64_t hwaddr;
#define HWADDR_PRIx PRIx64
#define MEMTXATTRS_UNSPECIFIED 0
#define MEMTX_OK 0
typedef struct {void *nsas;} IPodTouchMachineState;
typedef GError Error;
#define error_setg(ep, ...) g_set_error(ep,1,1,__VA_ARGS__)
static int writes, fail_write;
static unsigned char memory[16];
static int address_space_write(void *as, hwaddr addr, int attrs, const void *p, size_t n) {
    assert(addr==0x1234 && n<=sizeof(memory));
    writes++;
    if(fail_write)return 1;
    memcpy(memory,p,n);return 0;
}
''' + function + r'''
int main(int argc, char **argv) {
    IPodTouchMachineState s={0};Error *err=NULL;
    const char *path=argv[1];unsigned char bytes[17];
    for(unsigned i=0;i<sizeof(bytes);i++)bytes[i]=i;
    assert(!ipod_touch_stage_boot_image(&s,path,0x1234,16,&err));
    assert(err && !writes);g_clear_error(&err);
    assert(g_file_set_contents(path,"",0,NULL));
    assert(!ipod_touch_stage_boot_image(&s,path,0x1234,16,&err));
    assert(err && !writes);g_clear_error(&err);
    assert(g_file_set_contents(path,(char *)bytes,sizeof(bytes),NULL));
    assert(!ipod_touch_stage_boot_image(&s,path,0x1234,16,&err));
    assert(err && !writes);g_clear_error(&err);
    assert(g_file_set_contents(path,(char *)bytes,16,NULL));
    assert(ipod_touch_stage_boot_image(&s,path,0x1234,16,&err)==16);
    assert(!err && writes==1 && !memcmp(memory,bytes,16));
    fail_write=1;
    assert(!ipod_touch_stage_boot_image(&s,path,0x1234,16,&err));
    assert(err && writes==2);g_clear_error(&err);
    puts("PASS: missing, empty, oversized, exact-fit boot images and guest-write failure");
}
'''
with tempfile.TemporaryDirectory(prefix='it-boot-image-') as tmp:
    tmp = Path(tmp)
    (tmp / 'check.c').write_text(code)
    flags = shlex.split(subprocess.check_output(['pkg-config', '--cflags', '--libs', 'glib-2.0'], text=True))
    subprocess.run(['cc', '-fsanitize=address,undefined', str(tmp / 'check.c'), *flags, '-o', str(tmp / 'check')], check=True)
    subprocess.run([str(tmp / 'check'), str(tmp / 'image')], check=True)

# Optional real machine rejection checks; no guest instructions execute.
import os
import sys
if '--native' in sys.argv:
    qemu = root / 'build-native14/qemu-build/qemu-system-arm'
    files = root.parent / 'qemu-ios-files'
    with tempfile.TemporaryDirectory(prefix='it-boot-reject-') as tmp:
        tmp = Path(tmp)
        empty = tmp / 'empty'; empty.touch()
        large = tmp / 'large'; large.write_bytes(b'\0' * (0x100000 + 1))
        missing = tmp / 'missing'
        for name, bootrom, env in [
            ('bootrom-empty', empty, {}), ('bootrom-large', large, {}),
            ('iboot-missing', files/'bootrom_240_4', {'IT_DIRECT_IBOOT':str(missing)}),
            ('iboot-empty', files/'bootrom_240_4', {'IT_DIRECT_IBOOT':str(empty)}),
            ('iboot-large', files/'bootrom_240_4', {'IT_DIRECT_IBOOT':str(large)}),
            ('llb-large', files/'bootrom_240_4', {'IT_DIRECT_IBOOT':str(empty),'IT_DIRECT_LLB':str(large)}),
        ]:
            machine = f'iPod-Touch,bootrom={bootrom},nand={files}/nand,nor={files}/nor_n72ap.bin,nandrw={tmp}/overlay'
            result = subprocess.run([str(qemu), '-S', '-M', machine, '-m', '128M', '-display', 'none',
                '-serial', 'null', '-monitor', 'none'], capture_output=True, text=True, timeout=15,
                env={k:v for k,v in os.environ.items() if not k.startswith('IT_')} | env)
            assert result.returncode != 0, name
            assert 'boot image' in result.stderr.lower(), result.stderr
            print('PASS:', name)
