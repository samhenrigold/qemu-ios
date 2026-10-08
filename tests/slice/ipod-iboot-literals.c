/* hw/arm/it_iboot.c finds its targets by pattern in every iPod touch 2G iBoot, 2.1.1 .. 4.2.1.
 *
 * Host only. Each build's iBoot comes from the decrypted cache
 * (~/Developer/qemu-ios-files/ipod-ipsw/cache/<ipsw sha1>/iBoot.bin) or, failing that, is decrypted here from the
 * app's IPSW cache (~/Library/Caches/gold.samhenri.LightTouchMac/IPSW/<sha1>.ipsw) with the public iBoot key;
 * builds with neither are reported and skipped. The pinned answers: the security epoch miu_init demands of SYSIC
 * POWER_ID[31:24], and gBootArgs.commandLine for the 2.x NAND-boot data write (a literal only in iBoot-385; 0 = the
 * later iBoots pass it in a register, and nothing may be written). Synthetic images cover the refusals: absent,
 * ambiguous, out of window, not loaded by Thumb code. The iPod touch 1G's iBoot-204 (8900 + IMG2, key 0x837) pins
 * only the epoch.
 *
 * SLICE hw/arm/it_iboot.c range static uint16_t iboot_u16( | #ifndef IT_IBOOT_HOST_TEST
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <CommonCrypto/CommonCryptor.h>
#include "slice.h"
static uint32_t le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static bool contains(const uint8_t *p, size_t n, const char *s) { return memmem(p, n, s, strlen(s)) != NULL; }
static char *home_path(const char *fmt, const char *arg) {
 static char path[1024], rel[512];
 snprintf(rel, sizeof(rel), fmt, arg); snprintf(path, sizeof(path), "%s/%s", getenv("HOME"), rel); return path;
}
static uint8_t *slurp(FILE *f, size_t *len) {
 size_t cap = 1 << 20, n = 0, got; uint8_t *buf = malloc(cap);
 while ((got = fread(buf + n, 1, cap - n, f)) > 0) { n += got; if (n == cap) buf = realloc(buf, cap *= 2); }
 *len = n; return buf;
}
static uint8_t *read_file(const char *path, size_t *len) {
 FILE *f = fopen(path, "rb"); if (!f) return NULL;
 uint8_t *buf = slurp(f, len); fclose(f); return buf;
}
/* one member of an IPSW in the app's cache, by unzip pattern; NULL when the IPSW is not cached */
static uint8_t *ipsw_member(const char *sha1, const char *pattern, size_t *len) {
 const char *ipsw = home_path("Library/Caches/gold.samhenri.LightTouchMac/IPSW/%s.ipsw", sha1);
 if (access(ipsw, R_OK)) return NULL;
 char cmd[1200]; snprintf(cmd, sizeof(cmd), "unzip -p '%s' '%s'", ipsw, pattern);
 FILE *p = popen(cmd, "r"); assert(p); uint8_t *buf = slurp(p, len); assert(pclose(p) == 0 && *len); return buf;
}
/* the IMG3 DATA tag's payload offset and length */
static bool img3_data(const uint8_t *c, size_t n, size_t *off, size_t *dlen) {
 for (size_t at = 20; at + 12 <= n; at += le32(c + at + 4)) {
  if (le32(c + at + 4) < 12) return false;
  if (!memcmp(c + at, "ATAD", 4)) { *off = at + 12; *dlen = le32(c + at + 8); return *off + *dlen <= n; }
 }
 return false;
}
static void hex(const char *s, uint8_t *out, size_t n) { for (size_t i = 0; i < n; i++) sscanf(s + 2 * i, "%2hhx", &out[i]); }
static void aes_cbc(const uint8_t *in, size_t n, uint8_t *out, const char *iv_hex, const char *key_hex) {
 uint8_t iv[16], key[32]; size_t klen = strlen(key_hex) / 2, moved;
 hex(iv_hex, iv, 16); hex(key_hex, key, klen);
 assert(CCCrypt(kCCDecrypt, kCCAlgorithmAES, 0, key, klen, iv, in, n, out, n, &moved) == kCCSuccess && moved == n);
}

#define BASE 0x0ff00000u
static void put16(uint8_t *p, uint16_t v){p[0]=v;p[1]=v>>8;}
static void put32(uint8_t *p, uint32_t v){put16(p,v);put16(p+2,v>>16);}
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

/* build, IPSW sha1, iBoot IV, iBoot key, iBoot version, epoch, gBootArgs.commandLine */
static const struct { const char *build, *sha1, *iv, *key, *version; unsigned epoch; uint32_t cmdline; } BUILDS[] = {
 {"5F138", "c3c700be49ad227d1152188e7c1e46b8958fd1e4", "b3633afbe02e0e9ba4d7366c47abe5a8", "2d916dabb6dfd4594dbe3635b4c71662", "iBoot-385.22", 1, 0x0ff2a584},
 {"5G77a", "34a0a489605f34d6cc6c9954edcaaf9a050deedc", "c9a400fab94cb516953a6e30c7c17662", "a64b02ed6808452f7283bbea5dba2411", "iBoot-385.49", 2, 0x0ff2a584},
 {"5H11a", "9af5625ea34acdd8abeb6fce71a72651d0c815d5", "f7b095d291501e74093416fc2453a910", "ec5aabc0a54a268caf0e5dab337220ee", "iBoot-385.49", 2, 0x0ff2a584},
 {"7A341", "0f7fc76d9b9aa826b5ab14be9821a315d3d9dc42", "c71876986992913eeb8b12b072e00293", "e0476a04b7dfba9531e1c0263f8b0143", "iBoot-596.24", 3, 0},
 {"7C145", "e0d8800a4fc7cc5be6976ddbceb43c2d2a7120d7", "b31c608013ab8980178b79a1d8ba21df", "edebb477d07161edeac17b12c33ba783", "iBoot-636.66", 4, 0},
 {"7D11",  "e7c83d4a5baec0e81816ae1cd1caf9a4dc38ebf0", "5e421f8ce8c811311bbbb8a734ec07ce", "191b6846543d7026b6f0d5247f030588", "iBoot-636.66", 4, 0},
 {"7E18",  "5f4f5c01eda2f811f73167e7d1f82dbeed82367b", "7c090ab8c8a0cbc95db1007b322fe960", "ca3893d43d9446cd2f9f3fd5371d02e9", "iBoot-636.66.33", 4, 0},
 {"8A293", "c026c373bc535496a6f901de2ba37d4a487413bf", "892d0889ac3971254ced87e974fcff54", "cf0ce641bb5d00f703d8265fffb45c59", "iBoot-889.24", 4, 0},
 {"8A400", "06a42297d94461264eb64d7c8640cc5d1c19edeb", "892d0889ac3971254ced87e974fcff54", "cf0ce641bb5d00f703d8265fffb45c59", "iBoot-889.24", 4, 0},
 {"8B117", "97abde6207660bd876fd476275dd526d0dcf3d19", "07751c86d421a18d427ac7f94a74d747", "0359d66dd638e5c87e83b4e4daa941bf", "iBoot-931.18.27", 4, 0},
 {"8C148", "b9efddc7bb4350c237a8d3846af61bbfc8a2f647", "fcd3e0675b376f67e95328a2c691d363", "7a105db73b007a24ef18c9226452d761", "iBoot-931.71.16", 4, 0},
};
/* the iPod touch 1G: build, IPSW sha1, iBoot version, epoch */
static const struct { const char *build, *sha1, *version; unsigned epoch; } N45_BUILDS[] = {
 {"3A101a", "9b0d83c7f8b4328174a3f31e0e93f60e591ae143", "iBoot-204", 2},
 {"3A110a", "84bbc6ea8bf29745195bc9926c1874f7c2a36f32", "iBoot-204", 2},
 {"3B48b",  "108d8ffe9ea75e61cd5e57170ad388b7fa00d923", "iBoot-204", 2},
 {"4A93",   "8dca23eec69d5ae58fbf3d4a23276e46cbb2e3c6", "iBoot-204", 3},
 {"4A102",  "c148d1eb1c979bb6434175411d4a372103a4fdd2", "iBoot-204", 3},
 {"4B1",    "1b818911316e4248ee01d3ec67f9d39afc3db240", "iBoot-204.3.16", 3},
};
#define N(a) (sizeof(a) / sizeof((a)[0]))

/* imgtools' decrypted cache, else the img3 from the app's IPSW cache (3.x+ form: the last partial block encrypted) */
static uint8_t *n72_iboot(const char *sha1, const char *iv, const char *key, size_t *len) {
 uint8_t *image = read_file(home_path("Developer/qemu-ios-files/ipod-ipsw/cache/%s/iBoot.bin", sha1), len);
 if (image) return image;
 size_t n; uint8_t *c = ipsw_member(sha1, "*iBoot.n72ap.RELEASE.img3", &n);
 if (!c) return NULL;
 size_t off, dlen; assert(img3_data(c, n, &off, &dlen));
 size_t padded = (dlen + 15) & ~(size_t)15; assert(off + padded <= n);
 image = malloc(padded); aes_cbc(c + off, padded, image, iv, key); free(c);
 *len = dlen; return image;
}

/* all_flash iBoot: 8900 format 3 (AES-128-CBC, key 0x837, zero IV; a partial last block in the clear), then IMG2 */
static uint8_t *n45_iboot(const char *sha1, size_t *len) {
 size_t n; uint8_t *c = ipsw_member(sha1, "*/iBoot.n45ap.RELEASE.img2", &n);
 if (!c) return NULL;
 assert(n >= 0x800 && !memcmp(c, "8900", 4) && c[7] == 3);   /* an encrypted 8900 container */
 size_t blen = le32(c + 0xc); assert(0x800 + blen <= n);
 uint8_t *body = malloc(blen); size_t whole = blen & ~(size_t)15;
 aes_cbc(c + 0x800, whole, body, "00000000000000000000000000000000", "188458A6D15034DFE386F23B61D43774");
 memcpy(body + whole, c + 0x800 + whole, blen - whole); free(c);
 assert(blen >= 0x400 && !memcmp(body, "2gmI", 4));            /* an IMG2 image */
 size_t ilen = le32(body + 0x14); assert(0x400 + ilen <= blen);
 uint8_t *image = malloc(ilen); memcpy(image, body + 0x400, ilen); free(body);
 *len = ilen; return image;
}

static uint8_t img[0x100000];
static void stage(const uint8_t *image, size_t len, size_t *n) {
 memset(img, 0, sizeof(img)); *n = len < sizeof(img) ? len : sizeof(img); memcpy(img, image, *n);
}

int main(void) {
 synthetic();
 unsigned seen = 0, seen1 = 0;
 for (size_t i = 0; i < N(BUILDS); i++) {
  size_t len, n; uint8_t *image = n72_iboot(BUILDS[i].sha1, BUILDS[i].iv, BUILDS[i].key, &len);
  if (!image) { printf("SKIP %s: no cached iBoot.bin and no %s.ipsw\n", BUILDS[i].build, BUILDS[i].sha1); continue; }
  assert(contains(image, len, BUILDS[i].version));
  stage(image, len, &n); free(image);
  /* the NAND-boot data write scans 0x40000 bytes of RAM (ipod_touch_fmss.c IBOOT_SCAN_LEN): the buffer is BSS past the image */
  size_t scan = n < 0x40000 ? 0x40000 : n;
  unsigned epoch = it_iboot_find_epoch(img, n); uint32_t cmdline = it_iboot_find_command_line(img, scan, BASE);
  if (epoch != BUILDS[i].epoch || cmdline != BUILDS[i].cmdline) {
   fprintf(stderr, "%s %s: got epoch %u command-line %x\n", BUILDS[i].build, BUILDS[i].version, epoch, cmdline); return 1;
  }
  printf("%-6s %-16s epoch %u command-line ", BUILDS[i].build, BUILDS[i].version, epoch);
  if (cmdline) printf("0x%08x\n", cmdline); else puts("-");
  seen++;
 }
 for (size_t i = 0; i < N(N45_BUILDS); i++) {
  size_t len, n; uint8_t *image = n45_iboot(N45_BUILDS[i].sha1, &len);
  if (!image) { printf("SKIP %s: no %s.ipsw\n", N45_BUILDS[i].build, N45_BUILDS[i].sha1); continue; }
  assert(contains(image, len, N45_BUILDS[i].version));
  stage(image, len, &n); free(image);
  unsigned epoch = it_iboot_find_epoch(img, n);
  if (epoch != N45_BUILDS[i].epoch) { fprintf(stderr, "%s: got epoch %u\n", N45_BUILDS[i].build, epoch); return 1; }
  printf("%-6s %-16s epoch %u\n", N45_BUILDS[i].build, N45_BUILDS[i].version, epoch);
  seen1++;
 }
 assert(seen && "no iBoot image found for any build");
 printf("PASS: it_iboot finders on %u/%zu iPod touch 2G and %u/%zu 1G iBoots, plus the synthetic refusals\n",
        seen, N(BUILDS), seen1, N(N45_BUILDS));
}
