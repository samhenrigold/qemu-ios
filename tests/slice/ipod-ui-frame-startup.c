/* Frame polling is safe before attach, including concurrent first readers.
 *
 * SLICE contrib/ios-app/qemu-ios-ui.c fn ios_init_frame_lock qemu_ios_ui_attach qemu_ios_ui_frame qemu_ios_ui_frame_size
 * PKG glib-2.0
 */
#include <assert.h>
#include <glib.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
typedef struct { pthread_mutex_t lock; bool initialized; } QemuMutex;
static void qemu_mutex_init(QemuMutex *m) { assert(!m->initialized); assert(!pthread_mutex_init(&m->lock, NULL)); m->initialized=true; }
static void qemu_mutex_lock(QemuMutex *m) { assert(m->initialized); assert(!pthread_mutex_lock(&m->lock)); }
static void qemu_mutex_unlock(QemuMutex *m) { assert(!pthread_mutex_unlock(&m->lock)); }
typedef void (*qemu_ios_frame_cb)(void *);
static struct {
 QemuMutex frame_lock;
 void *buf[3]; int published, width, height; uint64_t serial;
 qemu_ios_frame_cb cb; void *cb_opaque;
} ios;
#include "slice.h"
static void *poll(void *unused) {
 for (int i=0;i<10000;i++) {
  const void *pixels=(void *)1; int w=-1,h=-1; uint64_t serial=0;
  assert(!qemu_ios_ui_frame(&pixels,&w,&h,&serial));
  assert(pixels==(void *)1 && w==-1 && h==-1 && serial==0);
  qemu_ios_ui_frame_size(&w,&h); assert(w==0 && h==0);
 }
 return NULL;
}
int main(void) {
 pthread_t threads[8];
 for (int i=0;i<8;i++) assert(!pthread_create(&threads[i],NULL,poll,NULL));
 qemu_ios_ui_attach(NULL,NULL);
 for (int i=0;i<8;i++) assert(!pthread_join(threads[i],NULL));
 int pixel=42; ios.buf[0]=&pixel; ios.published=0; ios.width=1; ios.height=1; ios.serial=1;
 const void *p=NULL; int w=0,h=0; uint64_t serial=0;
 assert(qemu_ios_ui_frame(&p,&w,&h,&serial));
 assert(p==&pixel && w==1 && h==1 && serial==1);
 qemu_ios_ui_attach(NULL,NULL); /* no reinitialization or frame loss */
 assert(!qemu_ios_ui_frame(&p,&w,&h,&serial));
 puts("PASS: concurrent frame polls before attach, empty outputs and retained publication");
}
