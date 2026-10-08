/* Actual QEMU publication/getter against real QEMU atomic/seqlock primitives.
 *
 * SLICE contrib/ios-app/qemu-ios-ui.c fn ios_sequence_publish qemu_ios_ui_input_sequence_status
 * CFLAGS -O2 -Wall -Werror -Wno-unused-function -Wno-comment -I$ROOT/include -I$ROOT/contrib/ios-app
 * PKG glib-2.0
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <glib.h>
#include "qemu/typedefs.h"
#define qemu_build_not_reached() __builtin_trap()
#define coroutine_fn
#define qemu_build_assert(test) _Static_assert(test, "atomic-size")
#include "qemu/seqlock.h"
#include "qemu-ios-ui.h"
#include <pthread.h>
static uint64_t ios_sequence_id;
static int ios_sequence_status;
static QemuSeqLock ios_sequence_publication;
static bool done;
#include "slice.h"
static void *reader(void *arg) {
 while (!qatomic_read(&done)) {
  uint64_t id=qatomic_read(&ios_sequence_id);
  int status=qemu_ios_ui_input_sequence_status(id);
  assert(status==0 || status==(int)(id%3+1));
 }
 return NULL;
}
int main(void) {
 pthread_t threads[4];
 for(int i=0;i<4;i++)assert(!pthread_create(&threads[i],NULL,reader,NULL));
 for(uint64_t id=1;id<=500000;id++)ios_sequence_publish(id,id%3+1);
 qatomic_set(&done,true);
 for(int i=0;i<4;i++)assert(!pthread_join(threads[i],NULL));
 assert(qemu_ios_ui_input_sequence_status(500000)==500000%3+1);
 assert(qemu_ios_ui_input_sequence_status(1)==0);
 puts("PASS: 500000 actual ID/status publications with four concurrent readers, real QEMU seqlock/atomic primitives");
}
