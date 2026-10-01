/* Research-only MediaPlayer probe for test_media_artwork.py; never packaged. */
#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <unistd.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>

typedef void *ID;
static ID (*cls)(const char *), (*sel)(const char *);
static void *send;
#define CALL(ret, args) ((ret (*)args)send)
static ID m0(ID o, const char *s) { return CALL(ID,(ID,ID))(o,sel(s)); }
static ID m1(ID o, const char *s, ID a) { return CALL(ID,(ID,ID,ID))(o,sel(s),a); }
static ID str(const char *s) {
    return CALL(ID,(ID,ID,const char *))(cls("NSString"),sel("stringWithUTF8String:"),s);
}
__attribute__((naked)) void _start(void) {
    __asm__ volatile("ldr r0, [sp]\n\tadd r1, sp, #4\n\tb _main");
}
int main(int argc, char **argv) {
    if (argc == 2 && !strcmp(argv[1],"--setup")) {
        const char *paths[] = {"/var/mobile/Media/LightTouch", "/var/mobile/Media/LightTouch/artwork-fixture"};
        for (unsigned i=0; i<2; ++i) {
            if (mkdir(paths[i],0755) && errno != EEXIST) _exit(1);
            if (chown(paths[i],501,501)) _exit(1);
        }
        _exit(0);
    }
    if (setgid(501) || setuid(501)) _exit(1);
    setenv("HOME","/var/mobile",1);
    void *objc = dlopen("/usr/lib/libobjc.A.dylib",RTLD_NOW);
    if (!objc) _exit(1);
    cls=dlsym(objc,"objc_getClass"); sel=dlsym(objc,"sel_registerName"); send=dlsym(objc,"objc_msgSend");
    if (!cls || !sel || !send || !dlopen("/System/Library/Frameworks/Foundation.framework/Foundation",RTLD_NOW)) _exit(1);
    void *uikit = dlopen("/System/Library/Frameworks/UIKit.framework/UIKit",RTLD_NOW);
    if (!uikit || !dlopen("/System/Library/Frameworks/MediaPlayer.framework/MediaPlayer",RTLD_NOW)) _exit(1);
    ID (*png)(ID) = dlsym(uikit,"UIImagePNGRepresentation");
    if (!png) _exit(1);
    ID pool=m0(m0(cls("NSAutoreleasePool"),"alloc"),"init");
    ID items=m0(m0(cls("MPMediaQuery"),"songsQuery"),"items");
    unsigned n=CALL(unsigned,(ID,ID))(items,sel("count"));
    unsigned images=0;
    printf("songs=%u\n",n);
    for (unsigned i=0; i<n; ++i) {
        ID item=CALL(ID,(ID,ID,unsigned))(items,sel("objectAtIndex:"),i);
        ID title=m1(item,"valueForProperty:",str("title"));
        const char *text=CALL(const char *,(ID,ID))(title,sel("UTF8String"));
        ID art=m1(item,"valueForProperty:",str("artwork"));
        ID image=CALL(ID,(ID,ID,float,float))(art,sel("imageWithSize:"),320,320);
        ID data=image ? png(image) : NULL;
        unsigned length=CALL(unsigned,(ID,ID))(data,sel("length"));
        printf("title=%s artwork=%p image=%p bytes=%u\n",text ? text : "",art,image,length);
        if (length && CALL(int,(ID,ID,ID,int))(data,sel("writeToFile:atomically:"),str("/tmp/guest-artwork.png"),1)) ++images;
    }
    m0(pool,"drain");
    fflush(stdout);
    _exit(n == 1 && images == 1 ? 0 : 1);
}
