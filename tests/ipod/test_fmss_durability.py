from pathlib import Path
import argparse,re,shlex,subprocess,tempfile
p=argparse.ArgumentParser(description='Test actual FMSS durable page/erase publication and injected failures.');p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2]/'hw/arm/ipod_touch_fmss.c');a=p.parse_args();source=a.source.read_text()
functions=[]
for name in ('fmss_io_error','fmss_sync_directory','fmss_ensure_directory','fmss_block_marker_path','fmss_erase_block','fmss_store_page','fmss_complete'):
 m=re.search(r'^static [^\n]*\b'+name+r'\([^)]*\)\s*\{.*?^}',source,re.M|re.S)
 if not m and name in ('fmss_sync_directory','fmss_ensure_directory'):
  functions.append('static bool '+name+'(const char *dir){return true;}')
 else:
  assert m,name;functions.append(m.group())
pre=r'''
#include <glib.h>
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <inttypes.h>
#define NAND_BYTES_PER_PAGE 4096
#define NAND_BYTES_PER_SPARE 64
#define NAND_PAGES_PER_BLOCK 128
#define RUN_STATE_IO_ERROR 1
#define QEMU_CLOCK_VIRTUAL 0
#define qatomic_set(p,v) (*(p)=(v))
#define error_report(...) ((void)0)
typedef struct {char *nand_overlay;GHashTable *overlay_pages;GTree *phys_pages;uint32_t reg_cs_ctrl,reg_cs_irq_bit,reg_cs_irq_mask;} IPodTouchFMSSState;
static bool fmss_io_failed;
static unsigned dir_syncs,fail_dir_sync,stops,published,erased,irq_calls,removals;
static char events[1024];static unsigned event_count;
static bool erase_on,fail_rename,fail_file_sync;
static struct {uint64_t shadowed;} fmss_stats;
static void event(char c){assert(event_count<sizeof(events)-1);events[event_count++]=c;events[event_count]=0;}
static int test_fsync(int fd){struct stat st;assert(fstat(fd,&st)==0);bool dir=S_ISDIR(st.st_mode);event(dir?'D':'F');if((dir && ++dir_syncs==fail_dir_sync) || (!dir && fail_file_sync)){errno=EIO;return -1;}return fsync(fd);}
static int test_rename(const char *a,const char *b){event('R');if(fail_rename){errno=EIO;return -1;}return rename(a,b);}
static int test_remove(const char *p){if(g_str_has_suffix(p,".page")){event('P');removals++;}return remove(p);}
static int qemu_open(const char *p,int flags,void *err){assert(flags&O_DIRECTORY);return open(p,flags);}
static void vm_stop(int state){assert(state==1);stops++;}
static bool fmss_erase_on(void){return erase_on;}
static bool fmss_block_is_erased(IPodTouchFMSSState *s,uint32_t cs,uint32_t block){return true;}
static void fmss_remember_erased(IPodTouchFMSSState *s,uint32_t cs,uint32_t block){event('E');erased++;}
static void fmss_overlay_index(IPodTouchFMSSState *s){event('I');published++;}
static gpointer fmss_block_key(uint32_t cs,uint32_t page){return GUINT_TO_POINTER((cs<<24)|page);}
static bool fmss_trace_on(void){return false;}
static uint64_t qemu_clock_get_ns(int clock){return 0;}
static void fmss_update_irq(IPodTouchFMSSState *s){irq_calls++;}
#define fsync test_fsync
#define rename test_rename
#define remove test_remove
'''
tests=r'''
static void reset(void){dir_syncs=fail_dir_sync=stops=published=erased=irq_calls=removals=event_count=0;events[0]=0;fmss_io_failed=false;erase_on=fail_rename=fail_file_sync=false;}
static void put(const char *p){FILE *f=fopen(p,"wb");assert(f);assert(fputc(7,f)!=EOF);assert(fclose(f)==0);}
static void check_complete(IPodTouchFMSSState *s,bool success){fmss_complete(s);assert((s->reg_cs_irq_bit&1)==success);assert(irq_calls==(unsigned)success);}
static void store_test(const char *root,bool fail){
 g_autofree char *overlay=g_build_filename(root,fail?"store-fail":"store",NULL);g_autofree char *chip=g_build_filename(overlay,"cs0",NULL);assert(g_mkdir_with_parents(chip,0755)==0);
 IPodTouchFMSSState s={.nand_overlay=overlay};uint8_t data[4096],spare[64];memset(data,0x5a,sizeof(data));memset(spare,0x3c,sizeof(spare));reset();fail_dir_sync=fail?1:0;
 bool ok=fmss_store_page(&s,0,7,data,spare);assert(ok==!fail);assert(stops==(unsigned)fail);assert(published==(unsigned)!fail);check_complete(&s,ok);
 assert(!strncmp(events,"FRD",3));g_autofree char *page=g_build_filename(chip,"7.page",NULL);FILE *f=fopen(page,"rb");assert(f);uint8_t back[4160];assert(fread(back,1,sizeof(back),f)==sizeof(back));assert(fgetc(f)==EOF);assert(fclose(f)==0);assert(!memcmp(back,data,4096));assert(!memcmp(back+4096,spare,64));
}
static void erase_test(const char *root,unsigned failure){
 g_autofree char *name=g_strdup_printf("erase-%u",failure);g_autofree char *overlay=g_build_filename(root,name,NULL);g_autofree char *chip=g_build_filename(overlay,"cs0",NULL);assert(g_mkdir_with_parents(chip,0755)==0);g_autofree char *page=g_build_filename(chip,"7.page",NULL);put(page);
 IPodTouchFMSSState s={.nand_overlay=overlay};reset();fail_dir_sync=failure;bool ok=fmss_erase_block(&s,0,0);assert(ok==(failure==0));assert(erased==(unsigned)ok);assert(stops==(unsigned)!ok);check_complete(&s,ok);
 assert(!strncmp(events,"FRD",3));if(failure==1){assert(removals==0);assert(g_file_test(page,G_FILE_TEST_EXISTS));}else{assert(removals==128);assert(!g_file_test(page,G_FILE_TEST_EXISTS));char *last=strrchr(events,'D');assert(last && last>strrchr(events,'P'));if(ok)assert(last<strrchr(events,'E'));}
 g_autofree char *marker=g_build_filename(chip,"blk0.erased",NULL);assert(g_file_test(marker,G_FILE_TEST_EXISTS));
}
static void mkdir_test(const char *root,bool fail){
 g_autofree char *name=g_strdup_printf("nested-%u",fail);g_autofree char *dir=g_build_filename(root,name,"one","two",NULL);reset();fail_dir_sync=fail?1:0;assert(fmss_ensure_directory(dir)==!fail);assert(stops==(unsigned)fail);assert(dir_syncs==(fail?1u:3u));if(!fail){assert(g_file_test(dir,G_FILE_TEST_IS_DIR));assert(!strcmp(events,"DDD"));reset();assert(fmss_ensure_directory(dir));assert(!dir_syncs);}
}

static void publication_failure_test(const char *root,bool erase,bool file_failure){
 g_autofree char *name=g_strdup_printf("publication-%u-%u",erase,file_failure);g_autofree char *overlay=g_build_filename(root,name,NULL);g_autofree char *chip=g_build_filename(overlay,"cs0",NULL);assert(g_mkdir_with_parents(chip,0755)==0);g_autofree char *page=g_build_filename(chip,"7.page",NULL);put(page);IPodTouchFMSSState s={.nand_overlay=overlay};uint8_t data[4096]={0},spare[64]={0};reset();fail_file_sync=file_failure;fail_rename=!file_failure;
 bool ok=erase?fmss_erase_block(&s,0,0):fmss_store_page(&s,0,7,data,spare);assert(!ok && stops==1 && !published && !erased && !removals);check_complete(&s,false);assert(!dir_syncs);FILE *f=fopen(page,"rb");assert(f && fgetc(f)==7 && fgetc(f)==EOF);assert(fclose(f)==0);g_autofree char *marker=g_build_filename(chip,"blk0.erased",NULL);assert(!g_file_test(marker,G_FILE_TEST_EXISTS));
}
int main(int argc,char **argv){assert(argc==2);store_test(argv[1],false);store_test(argv[1],true);mkdir_test(argv[1],false);mkdir_test(argv[1],true);erase_test(argv[1],0);erase_test(argv[1],1);erase_test(argv[1],2);publication_failure_test(argv[1],false,false);publication_failure_test(argv[1],false,true);publication_failure_test(argv[1],true,false);publication_failure_test(argv[1],true,true);puts("PASS actual-source page/erase directory ordering, reopen, creation, injected failure no IRQ/cache publication");}
'''
with tempfile.TemporaryDirectory(prefix='fmss-durability-') as d:
 d=Path(d);(d/'test.c').write_text(pre+'\n'+'\n'.join(functions)+'\n'+tests)
 flags=shlex.split(subprocess.check_output(['pkg-config','--cflags','--libs','glib-2.0'],text=True));subprocess.run(['clang','-g','-fsanitize=address,undefined','-o',str(d/'test'),str(d/'test.c'),*flags],check=True);subprocess.run([str(d/'test'),str(d)],check=True)
