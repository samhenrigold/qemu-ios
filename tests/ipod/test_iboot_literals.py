#!/usr/bin/env python3
"""hw/arm/it_iboot.c finds its targets by pattern in every iPod touch 2G iBoot, 2.1.1 .. 4.2.1.

Host only. Each build's iBoot comes from imgtools/device.py's decrypted cache
(~/Developer/qemu-ios-files/ipod-ipsw/cache/<ipsw sha1>/iBoot.bin) or, failing that, is decrypted here from the
app's IPSW cache (~/Library/Caches/gold.samhenri.LightTouchMac/IPSW/<sha1>.ipsw) with the public iBoot key;
builds with neither are reported and skipped. The pinned answers: the normal-boot command-line literal the early
handoff redirects (IBOOT_MEM_BASE + offset), the security epoch miu_init demands of SYSIC POWER_ID[31:24], and
gBootArgs.commandLine for the 2.x NAND-boot data write (a literal only in iBoot-385; 0 = the later iBoots pass
it in a register, and nothing may be written). Synthetic images cover the refusals: absent, ambiguous, out of
window, not loaded by Thumb code.
"""
import hashlib, os, subprocess, sys, tempfile, zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'imgtools'))
BASE = 0x0ff00000
DEC = Path('~/Developer/qemu-ios-files/ipod-ipsw/cache').expanduser()
IPSW = Path('~/Library/Caches/gold.samhenri.LightTouchMac/IPSW').expanduser()

# build: (IPSW sha1, iBoot IV, iBoot key, iBoot version, boot-args literal, epoch, gBootArgs.commandLine)
BUILDS = {
    '5F138': ('c3c700be49ad227d1152188e7c1e46b8958fd1e4', 'b3633afbe02e0e9ba4d7366c47abe5a8', '2d916dabb6dfd4594dbe3635b4c71662', 'iBoot-385.22', 0x108b4, 1, 0x0ff2a584),
    '5G77a': ('34a0a489605f34d6cc6c9954edcaaf9a050deedc', 'c9a400fab94cb516953a6e30c7c17662', 'a64b02ed6808452f7283bbea5dba2411', 'iBoot-385.49', 0x108bc, 2, 0x0ff2a584),
    '5H11a': ('9af5625ea34acdd8abeb6fce71a72651d0c815d5', 'f7b095d291501e74093416fc2453a910', 'ec5aabc0a54a268caf0e5dab337220ee', 'iBoot-385.49', 0x108bc, 2, 0x0ff2a584),
    '7A341': ('0f7fc76d9b9aa826b5ab14be9821a315d3d9dc42', 'c71876986992913eeb8b12b072e00293', 'e0476a04b7dfba9531e1c0263f8b0143', 'iBoot-596.24', 0x10d4c, 3, 0),
    '7C145': ('e0d8800a4fc7cc5be6976ddbceb43c2d2a7120d7', 'b31c608013ab8980178b79a1d8ba21df', 'edebb477d07161edeac17b12c33ba783', 'iBoot-636.66', 0x11b28, 4, 0),
    '7D11':  ('e7c83d4a5baec0e81816ae1cd1caf9a4dc38ebf0', '5e421f8ce8c811311bbbb8a734ec07ce', '191b6846543d7026b6f0d5247f030588', 'iBoot-636.66', 0x11b28, 4, 0),
    '7E18':  ('5f4f5c01eda2f811f73167e7d1f82dbeed82367b', '7c090ab8c8a0cbc95db1007b322fe960', 'ca3893d43d9446cd2f9f3fd5371d02e9', 'iBoot-636.66.33', 0x11b28, 4, 0),
    '8A293': ('c026c373bc535496a6f901de2ba37d4a487413bf', '892d0889ac3971254ced87e974fcff54', 'cf0ce641bb5d00f703d8265fffb45c59', 'iBoot-889.24', 0xa1e0, 4, 0),
    '8A400': ('06a42297d94461264eb64d7c8640cc5d1c19edeb', '892d0889ac3971254ced87e974fcff54', 'cf0ce641bb5d00f703d8265fffb45c59', 'iBoot-889.24', 0xa1e0, 4, 0),
    '8B117': ('97abde6207660bd876fd476275dd526d0dcf3d19', '07751c86d421a18d427ac7f94a74d747', '0359d66dd638e5c87e83b4e4daa941bf', 'iBoot-931.18.27', 0xa278, 4, 0),
    '8C148': ('b9efddc7bb4350c237a8d3846af61bbfc8a2f647', 'fcd3e0675b376f67e95328a2c691d363', '7a105db73b007a24ef18c9226452d761', 'iBoot-931.71.16', 0xa190, 4, 0),
}

# The iPod touch 1G's iBoot-204 (8900 + IMG2, key 0x837): build -> (IPSW sha1, iBoot version, epoch); only the epoch.
N45_BUILDS = {
    '3A101a': ('9b0d83c7f8b4328174a3f31e0e93f60e591ae143', 'iBoot-204', 2),
    '3A110a': ('84bbc6ea8bf29745195bc9926c1874f7c2a36f32', 'iBoot-204', 2),
    '3B48b':  ('108d8ffe9ea75e61cd5e57170ad388b7fa00d923', 'iBoot-204', 2),
    '4A93':   ('8dca23eec69d5ae58fbf3d4a23276e46cbb2e3c6', 'iBoot-204', 3),
    '4A102':  ('c148d1eb1c979bb6434175411d4a372103a4fdd2', 'iBoot-204', 3),
    '4B1':    ('1b818911316e4248ee01d3ec67f9d39afc3db240', 'iBoot-204.3.16', 3),
}

HARNESS = r'''
#include "qemu/osdep.h"
#include "hw/arm/it_iboot.h"
#include <assert.h>
#include <stdio.h>
#define BASE 0x0ff00000u
static void put16(uint8_t *p, uint16_t v){p[0]=v;p[1]=v>>8;}
static void put32(uint8_t *p, uint32_t v){put16(p,v);put16(p+2,v>>16);}
/* ldr r0,[pc,#imm] at `ldr` loading word `e`: the empty-string literal, the restore literal right after it */
static void place(uint8_t *img, size_t ldr, size_t e, size_t str, size_t empty){
 static const char restore[]="rd=md0 nand-enable-reformat=1 -progress";
 img[ldr]=(uint8_t)((e-((ldr+4)&~3))/4); img[ldr+1]=0x48;
 put32(img+e,BASE+empty); put32(img+e+4,BASE+str); memcpy(img+str,restore,sizeof(restore));
}
static void epoch_fixture(uint8_t *img, size_t acc, size_t call, unsigned floor){
 static const uint8_t accessor[]={0x02,0x48,0x00,0x68,0x40,0x05,0x40,0x0e,0x70,0x47};
 memcpy(img+acc,accessor,sizeof(accessor)); put32(img+acc+12,0x3d100008);
 int32_t off=(int32_t)acc-(int32_t)(call+4);
 put16(img+call,0xf000|((off>>12)&0x7ff)); put16(img+call+2,0xf800|((off>>1)&0x7ff));
 put16(img+call+4,0x2800|(floor-1)); put16(img+call+6,0xd800); put16(img+call+8,0x2000|floor); put16(img+call+10,0xbd80);
}
static void command_line_fixture(uint8_t *image, uint32_t base){
 memset(image,0,1024);
 put16(image,0x4c0f);put16(image+2,0x4810);put16(image+4,0x1c21);put16(image+6,0xf000);put16(image+8,0xf800);
 put32(image+64,base+512);put32(image+68,base+128);
 memcpy(image+128,"gBootArgs.commandLine = [%s]\n",29);
}
static void synthetic(void){
 static uint8_t img[0x27000];
 /* 7E18's shape */
 place(img,0x11a7c,0x11b28,0x1c000,0x1dba0);
 assert(it_iboot_find_boot_args_literal(img,sizeof(img),BASE)==0x11b28);
 img[0x11a7c]^=1; assert(!it_iboot_find_boot_args_literal(img,sizeof(img),BASE)); img[0x11a7c]^=1;   /* no Thumb load */
 put32(img+0x11b28,BASE+sizeof(img)); assert(!it_iboot_find_boot_args_literal(img,sizeof(img),BASE)); /* outside */
 put32(img+0x11b28,BASE+0x1c000); assert(!it_iboot_find_boot_args_literal(img,sizeof(img),BASE));     /* not "" */
 put32(img+0x11b28,BASE+0x1dba0);
 put32(img+0x2000,BASE+0x1c000); assert(!it_iboot_find_boot_args_literal(img,sizeof(img),BASE));       /* two literals */
 put32(img+0x2000,0); memcpy(img+0x3000,img+0x1c000,40); assert(!it_iboot_find_boot_args_literal(img,sizeof(img),BASE)); /* two strings */
 memset(img+0x3000,0,40); assert(it_iboot_find_boot_args_literal(img,sizeof(img),BASE)==0x11b28);
 assert(!it_iboot_find_boot_args_literal(NULL,sizeof(img),BASE)&&!it_iboot_find_boot_args_literal(img,8,BASE));
 /* 8C148's shape elsewhere in the image: follows the literals, not an offset */
 memset(img,0,sizeof(img)); place(img,0xa100,0xa190,0x1ee68,0x1e4b4);
 assert(it_iboot_find_boot_args_literal(img,sizeof(img),BASE)==0xa190);
 /* epoch: floor after the one call into the fuse accessor */
 memset(img,0,sizeof(img)); epoch_fixture(img,0x143a0,0x1a958,4); assert(it_iboot_find_epoch(img,sizeof(img))==4);
 epoch_fixture(img,0x143a0,0x1a958,3); assert(it_iboot_find_epoch(img,sizeof(img))==3);
 put16(img+0x1a958+6,0xd100); assert(it_iboot_find_epoch(img,sizeof(img))==3);      /* bne form (385.22) */
 put16(img+0x1a958+8,0x2100); assert(!it_iboot_find_epoch(img,sizeof(img)));          /* movs r1: not the floor */
 epoch_fixture(img,0x143a0,0x1a958,3); epoch_fixture(img,0x143a0,0x1b000,4); assert(!it_iboot_find_epoch(img,sizeof(img))); /* ambiguous */
 memset(img,0,sizeof(img)); epoch_fixture(img,0x143a0,0x1a958,3); memcpy(img+0x2000,img+0x143a0,16); assert(!it_iboot_find_epoch(img,sizeof(img)));
 memset(img,0,sizeof(img)); assert(!it_iboot_find_epoch(img,sizeof(img))&&!it_iboot_find_epoch(NULL,16)&&!it_iboot_find_epoch(img,8));
 /* iBoot-204's inline compare: ldr r3,[r3]; lsrs r3,r3,#24; cmp r3,#M; beq; ldr r0/r1 = the panic text */
 memset(img,0,sizeof(img)); put16(img+0x1fb6,0x681b); put16(img+0x1fb8,0x0e1b); put16(img+0x1fba,0x2b03); put16(img+0x1fbc,0xd003);
 put16(img+0x1fbe,0x4905); put32(img+0x1fd4,0x1801a140); memcpy(img+0x1a140,"miu_init: Epoch Mismatch\n",25);
 assert(it_iboot_find_epoch(img,sizeof(img))==3);
 img[0x1a14a]='X'; assert(!it_iboot_find_epoch(img,sizeof(img))); img[0x1a14a]='E';                 /* not the panic */
 put16(img+0x1fb8,0x0e1a); assert(!it_iboot_find_epoch(img,sizeof(img))); put16(img+0x1fb8,0x0e1b); /* another register */
 memcpy(img+0x3000,img+0x1fb6,10); put32(img+0x3020,0x1801a140); assert(!it_iboot_find_epoch(img,sizeof(img))); /* ambiguous */
 /* the 2.x command-line literal pair */
 uint8_t image[1024];uint32_t base=0x10000000;
 command_line_fixture(image,base); assert(it_iboot_find_command_line(image,sizeof(image),base)==base+512);
 command_line_fixture(image,base+0x1000); assert(it_iboot_find_command_line(image,sizeof(image),base+0x1000)==base+0x1200);
 command_line_fixture(image,base);image[128]='X'; assert(!it_iboot_find_command_line(image,sizeof(image),base));
 command_line_fixture(image,base);put32(image+64,base+900); assert(!it_iboot_find_command_line(image,sizeof(image),base));
 command_line_fixture(image,base);put32(image+68,base-4); assert(!it_iboot_find_command_line(image,sizeof(image),base));
 command_line_fixture(image,base);image[4]^=1; assert(!it_iboot_find_command_line(image,sizeof(image),base));
 command_line_fixture(image,base);memcpy(image+16,image,10);put16(image+16,0x4c0b);put16(image+18,0x480c);
 assert(!it_iboot_find_command_line(image,sizeof(image),base));
 assert(!it_iboot_find_command_line(NULL,1024,base));
 command_line_fixture(image,base); for(size_t n=0;n<768;n++)assert(!it_iboot_find_command_line(image,n,base));
 assert(!it_iboot_find_command_line(image,1024,UINT32_MAX-512));
}
int main(int argc,char **argv){
 if(argc<2){synthetic();puts("synthetic ok");return 0;}
 FILE *f=fopen(argv[1],"rb"); assert(f); static uint8_t img[0x100000]; size_t n=fread(img,1,sizeof(img),f); fclose(f);
 /* the NAND-boot data write scans 0x40000 bytes of RAM (ipod_touch_fmss.c IBOOT_SCAN_LEN): the buffer is BSS past the image */
 size_t scan=n<0x40000?0x40000:n;
 printf("%x %u %x\n",it_iboot_find_boot_args_literal(img,n,BASE),it_iboot_find_epoch(img,n),it_iboot_find_command_line(img,scan,BASE));
 return 0;
}
'''


def iboot_image(build, sha1, iv, key):
    dec = DEC / sha1 / 'iBoot.bin'
    if dec.exists():
        return dec.read_bytes()
    ipsw = IPSW / (sha1 + '.ipsw')
    if not ipsw.exists():
        return None
    from ipad1_fw import img3_decrypt
    z = zipfile.ZipFile(ipsw)
    member = [n for n in z.namelist() if n.endswith('iBoot.n72ap.RELEASE.img3')][0]
    return img3_decrypt(z.read(member), bytes.fromhex(iv), bytes.fromhex(key))


def n45_iboot(sha1):
    """all_flash iBoot: 8900 format 3 (AES-128-CBC, key 0x837, zero IV; a partial last block in the clear), then IMG2."""
    ipsw = IPSW / (sha1 + '.ipsw')
    if not ipsw.exists():
        return None
    z = zipfile.ZipFile(ipsw)
    c = z.read([n for n in z.namelist() if n.endswith('/iBoot.n45ap.RELEASE.img2')][0])
    assert c[:4] == b'8900' and c[7] == 3, 'not an encrypted 8900 container'
    body = c[0x800:0x800 + int.from_bytes(c[0xc:0x10], 'little')]
    n = len(body) & ~15
    body = subprocess.run(['openssl', 'enc', '-d', '-aes-128-cbc', '-nopad', '-K', '188458A6D15034DFE386F23B61D43774',
                           '-iv', '0' * 32], input=body[:n], capture_output=True, check=True).stdout + body[n:]
    assert body[:4] == b'2gmI', 'not an IMG2 image'
    return body[0x400:0x400 + int.from_bytes(body[0x14:0x18], 'little')]


with tempfile.TemporaryDirectory() as temp:
    p = Path(temp)
    (p / 'qemu').mkdir()
    (p / 'qemu/osdep.h').write_text('#pragma once\n#include <stdbool.h>\n#include <stdint.h>\n#include <stdlib.h>\n#include <string.h>\n')
    (p / 'check.c').write_text(HARNESS)
    subprocess.run(['clang', '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-DIT_IBOOT_HOST_TEST',
                    '-I' + str(p), '-I' + str(ROOT / 'include'), str(p / 'check.c'), str(ROOT / 'hw/arm/it_iboot.c'),
                    '-o', str(p / 'check')], check=True)
    subprocess.run([str(p / 'check')], check=True)
    seen = 0
    for build, (sha1, iv, key, version, literal, epoch, cmdline) in sorted(BUILDS.items()):
        image = iboot_image(build, sha1, iv, key)
        if image is None:
            print('SKIP %s: no %s/iBoot.bin and no %s.ipsw' % (build, DEC / sha1, IPSW / sha1))
            continue
        assert version.encode() in image, (build, version)
        (p / 'iboot.bin').write_bytes(image)
        got = subprocess.run([str(p / 'check'), str(p / 'iboot.bin')], check=True, capture_output=True, text=True).stdout.split()
        want = ['%x' % literal, '%u' % epoch, '%x' % cmdline]
        assert got == want, (build, version, got, want)
        print('%-6s %-16s literal 0x%08x epoch %d command-line %s' % (build, version, BASE + literal, epoch, '0x%08x' % cmdline if cmdline else '-'))
        seen += 1
    seen1 = 0
    for build, (sha1, version, epoch) in sorted(N45_BUILDS.items()):
        image = n45_iboot(sha1)
        if image is None:
            print('SKIP %s: no %s.ipsw' % (build, IPSW / sha1))
            continue
        assert version.encode() in image, (build, version)
        (p / 'iboot.bin').write_bytes(image)
        got = subprocess.run([str(p / 'check'), str(p / 'iboot.bin')], check=True, capture_output=True, text=True).stdout.split()
        assert got[1] == '%u' % epoch, (build, version, got, epoch)
        print('%-6s %-16s epoch %d' % (build, version, epoch))
        seen1 += 1
    assert seen, 'no iBoot image found for any build'
    print('PASS: it_iboot finders on %d/%d iPod touch 2G and %d/%d 1G iBoots, plus the synthetic refusals'
          % (seen, len(BUILDS), seen1, len(N45_BUILDS)))
