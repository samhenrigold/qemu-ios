/* it_iboot_find_miu_epoch reads the boot security epoch off every iPad 1 iBoot, 817 .. 1219.
 *
 * Host only. iBoot's miu_init demands POWER_ID[31:24] == epoch(), the CHIPID fuse field floored at the build's
 * epoch; on hardware LLB writes that byte, and the machine's iboot= path (which skips LLB) writes what LLB would.
 * Images come from the iPad 1 decrypt caches (~/Developer/qemu-ios-files/ipad1/repro/cache/<ipsw sha1>/iBoot.bin)
 * or the iOS 5 spike's (~/Developer/qemu-ios-files/ios5-spike/dec-<build>/); missing builds are skipped. Pinned: the
 * model's fuse field (1, docs/ipad1/hw1-probes.log) gives 1 for 3.2/4.2 and 2 from 4.3; a fuse above the floor
 * wins; two copies of the check refuse.
 *
 * SLICE hw/arm/it_iboot.c range static uint16_t iboot_u16( | #ifndef IT_IBOOT_HOST_TEST
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "slice.h"

/* build: image candidates, iBoot version, epoch for fuse 0, 1, 3 */
static const struct { const char *build, *dirs[2], *version; unsigned want[3]; } BUILDS[] = {
 {"7B500", {"ipad1/repro/cache/68b613f78581d36eab96aa5a007001dff142baa3", "ipad1/7B500/dec"}, "iBoot-817.29", {1, 1, 3}},
 {"8C148", {"ipad1/repro/cache/8717b3bedc925b587566442ad375aa65d857e79a"}, "iBoot-931.71.16", {1, 1, 3}},
 {"8L1",   {"ios5-spike/dec-8L1"}, "iBoot-1072.61", {2, 2, 3}},
 {"9A405", {"ios5-spike/dec-9A405"}, "iBoot-1219.43.32", {2, 2, 3}},
 {"9B206", {"ipad1/repro/cache/ad9b607439250f2337fe132890dadc4c487beca8", "ios5-spike/dec-9B206"}, "iBoot-1219.62.15", {2, 2, 3}},
};
#define N(a) (sizeof(a) / sizeof((a)[0]))

int main(void) {
 static uint8_t img[0x200000];
 unsigned seen = 0;
 for (size_t i = 0; i < N(BUILDS); i++) {
  FILE *f = NULL;
  for (size_t d = 0; d < 2 && !f && BUILDS[i].dirs[d]; d++) {
   char path[1024]; snprintf(path, sizeof(path), "%s/Developer/qemu-ios-files/%s/iBoot.bin", getenv("HOME"), BUILDS[i].dirs[d]);
   f = fopen(path, "rb");
  }
  if (!f) { printf("SKIP %s: no decrypted iBoot\n", BUILDS[i].build); continue; }
  memset(img, 0, sizeof(img));
  size_t n = fread(img, 1, 0x100000, f); fclose(f);
  assert(memmem(img, n, BUILDS[i].version, strlen(BUILDS[i].version)));
  unsigned got[4] = {it_iboot_find_miu_epoch(img, n, 0), it_iboot_find_miu_epoch(img, n, 1), it_iboot_find_miu_epoch(img, n, 3)};
  memcpy(img + n, img, n);   /* the check twice: ambiguous */
  got[3] = it_iboot_find_miu_epoch(img, 2 * n, 1);
  if (memcmp(got, BUILDS[i].want, sizeof(BUILDS[i].want)) || got[3]) {
   fprintf(stderr, "%s: got %u %u %u %u\n", BUILDS[i].build, got[0], got[1], got[2], got[3]); return 1;
  }
  printf("%-6s %-17s epoch %u (fuse 0: %u, fuse 3: %u)\n", BUILDS[i].build, BUILDS[i].version, got[1], got[0], got[2]);
  seen++;
 }
 assert(seen && "no iBoot image found for any build");
 printf("PASS: it_iboot_find_miu_epoch on %u/%zu iPad 1 iBoots\n", seen, N(BUILDS));
}
