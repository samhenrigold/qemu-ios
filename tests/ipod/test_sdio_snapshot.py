#!/usr/bin/env python3
"""Sanitizer checks for production sparse-memory and queued-frame serialization."""
from pathlib import Path
import shlex
import subprocess
import tempfile
root = Path(__file__).resolve().parents[2]
code = r'''
#include <glib.h>
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <string.h>
typedef void VMStateField;
typedef void JSONWriter;
typedef struct { GByteArray *bytes; size_t pos; int error; } QEMUFile;
static int qemu_file_get_error(QEMUFile *f) { return f->error; }
static void qemu_put_buffer(QEMUFile *f,const uint8_t *p,size_t n) { g_byte_array_append(f->bytes,p,n); }
static size_t qemu_get_buffer(QEMUFile *f,uint8_t *p,size_t n) {
 size_t avail=f->bytes->len-f->pos;
 if(n>avail){n=avail;f->error=-EIO;}
 memcpy(p,f->bytes->data+f->pos,n);f->pos+=n;return n;
}
static void qemu_put_be32(QEMUFile *f,uint32_t n) { n=GUINT32_TO_BE(n);qemu_put_buffer(f,(void *)&n,4); }
static uint32_t qemu_get_be32(QEMUFile *f) { uint32_t n=0;qemu_get_buffer(f,(void *)&n,4);return GUINT32_FROM_BE(n); }
#define BACKPLANE_PAGE_BITS 12
#define BACKPLANE_PAGE_SIZE 4096
typedef struct { uint8_t *data; uint32_t len,read_off; } SDPCMFrame;
#include "hw/arm/ipod-sdio-state.h"
static void reset(QEMUFile *f) { g_byte_array_set_size(f->bytes,0);f->pos=0;f->error=0; }
int main(void) {
 QEMUFile f={.bytes=g_byte_array_new()};
 GHashTable *a=g_hash_table_new_full(g_direct_hash,g_direct_equal,NULL,g_free);
 GHashTable *b=g_hash_table_new_full(g_direct_hash,g_direct_equal,NULL,g_free);
 uint8_t page[4096];for(unsigned i=0;i<sizeof(page);i++)page[i]=i;
 g_hash_table_insert(a,GUINT_TO_POINTER(0),g_memdup2(page,sizeof(page)));
 g_hash_table_insert(a,GUINT_TO_POINTER(0xfffff),g_memdup2(page,sizeof(page)));
 assert(!sdio_put_backplane(&f,&a,0,NULL,NULL));
 assert(!sdio_get_backplane(&f,&b,0,NULL));
 assert(g_hash_table_size(b)==2);
 assert(!memcmp(page,g_hash_table_lookup(b,GUINT_TO_POINTER(0)),4096));
 reset(&f);qemu_put_be32(&f,4097);assert(sdio_get_backplane(&f,&b,0,NULL)==-EINVAL);
 reset(&f);qemu_put_be32(&f,1);qemu_put_be32(&f,0x100000);assert(sdio_get_backplane(&f,&b,0,NULL)==-EINVAL);
 reset(&f);qemu_put_be32(&f,2);
 for(int i=0;i<2;i++){qemu_put_be32(&f,7);qemu_put_buffer(&f,page,4096);}
 assert(sdio_get_backplane(&f,&b,0,NULL)==-EINVAL);
 reset(&f);qemu_put_be32(&f,1);qemu_put_be32(&f,7);qemu_put_buffer(&f,page,100);
 assert(sdio_get_backplane(&f,&b,0,NULL)==-EIO);
 GQueue *q=g_queue_new(),*r=g_queue_new();
 SDPCMFrame *frame=g_new0(SDPCMFrame,1);frame->len=4096;frame->read_off=1031;frame->data=g_memdup2(page,4096);g_queue_push_tail(q,frame);
 reset(&f);assert(!sdio_put_frames(&f,&q,0,NULL,NULL));assert(!sdio_get_frames(&f,&r,0,NULL));
 frame=g_queue_peek_head(r);assert(frame->len==4096&&frame->read_off==1031&&!memcmp(frame->data,page,4096));
 reset(&f);qemu_put_be32(&f,257);assert(sdio_get_frames(&f,&r,0,NULL)==-EINVAL);
 reset(&f);qemu_put_be32(&f,1);qemu_put_be32(&f,65537);qemu_put_be32(&f,0);assert(sdio_get_frames(&f,&r,0,NULL)==-EINVAL);
 reset(&f);qemu_put_be32(&f,1);qemu_put_be32(&f,4);qemu_put_be32(&f,4);assert(sdio_get_frames(&f,&r,0,NULL)==-EINVAL);
 reset(&f);qemu_put_be32(&f,1);qemu_put_be32(&f,4);qemu_put_be32(&f,0);assert(sdio_get_frames(&f,&r,0,NULL)==-EIO);
 g_queue_free_full(q,sdio_free_frame);g_queue_free_full(r,sdio_free_frame);
 g_hash_table_unref(a);g_hash_table_unref(b);g_byte_array_unref(f.bytes);
 g_print("PASS: sparse pages, partial frames, duplicate keys, bounds and truncated snapshots\n");
}
'''
with tempfile.TemporaryDirectory() as d:
    source=Path(d)/'check.c'; source.write_text(code)
    flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True))
    subprocess.run(['clang','-g','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root),str(source),'-o',d+'/check',*flags],check=True)
    subprocess.run([d+'/check'],check=True)
