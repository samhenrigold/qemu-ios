/* A bounded UIImageWriteToSavedPhotosAlbum client for 7E18. A persistent
 * receipt prevents replay when a previous process died during an async save. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <dlfcn.h>
#include <errno.h>

#define ROOT "/var/mobile/Media/LightTouch"
typedef void *ID;
static ID (*getclass)(const char *), (*selector)(const char *);
static void *send;
static char photo_path[256], receipt_path[256];
#define CALL(ret,args) ((ret (*)args)send)
static ID m0(ID o, const char *s) { return CALL(ID,(ID,ID))(o,selector(s)); }
static ID m1(ID o, const char *s, ID value) { return CALL(ID,(ID,ID,ID))(o,selector(s),value); }
static ID string(const char *s) {
    return CALL(ID,(ID,ID,const char *))(getclass("NSString"),selector("stringWithUTF8String:"),s);
}
static const char *utf8(ID o) { return CALL(const char *,(ID,ID))(o,selector("UTF8String")); }
static void fail(const char *reason) {
    fprintf(stderr,"itphoto: %s\n",reason);
    _exit(1);
}
static void complete(void) {
    puts("already-imported");
    fflush(stdout);
    _exit(0);
}
static int component(const char *s) {
    size_t length = strlen(s);
    if (!length || length > 128 || s[0] == '.') return 0;
    for (; *s; ++s) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') ||
              (*s >= '0' && *s <= '9') || *s == '-' || *s == '_' || *s == '.')) return 0;
    }
    return 1;
}
static void directory(const char *path) {
    struct stat st;
    if (lstat(path,&st) || !S_ISDIR(st.st_mode)) fail("invalid staging directory");
}
static void receipt(int first) {
    int flags = O_WRONLY|O_NOFOLLOW|O_NONBLOCK|(first ? O_CREAT|O_EXCL : O_TRUNC);
    int fd = open(receipt_path,flags,0600);
    if (fd < 0) fail("cannot write import receipt; outcome may be unknown");
    struct stat st;
    if (fstat(fd,&st) || !S_ISREG(st.st_mode)) fail("invalid import receipt");
    const char *text = first ? "pending\n" : "done\n";
    size_t length = strlen(text), offset = 0;
    while (offset < length) {
        ssize_t count = write(fd,text+offset,length-offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) fail("cannot finish import receipt; outcome may be unknown");
        offset += count;
    }
    if (fsync(fd)) fail("cannot flush import receipt; outcome may be unknown");
    close(fd);
}

/* The host supplies a baseline JPEG, already oriented and downsampled. Check
 * dimensions before asking UIKit to decode it in the 128 MiB guest. */
static int jpeg_size(FILE *file) {
    if (fgetc(file) != 0xff || fgetc(file) != 0xd8) return 0;
    for (;;) {
        if (fgetc(file) != 0xff) return 0;
        int marker;
        do { marker = fgetc(file); } while (marker == 0xff);
        if (marker <= 0 || marker == 0xda || marker == 0xd9) return 0;
        int hi = fgetc(file), lo = fgetc(file);
        if (hi < 0 || lo < 0) return 0;
        int length = (hi << 8) | lo;
        if (length < 2) return 0;
        if (marker == 0xc0) {
            unsigned char frame[6];
            if (length < 8 || fread(frame,1,sizeof(frame),file) != sizeof(frame)) return 0;
            int height = (frame[1] << 8) | frame[2];
            int width = (frame[3] << 8) | frame[4];
            return frame[0] == 8 && width > 0 && width <= 2048 && height > 0 && height <= 2048
                && (frame[5] == 1 || frame[5] == 3) && length == 8 + 3*frame[5];
        }
        if ((marker >= 0xc1 && marker <= 0xc3) || (marker >= 0xc5 && marker <= 0xcf && marker != 0xc8 && marker != 0xcc))
            return 0;
        if (fseek(file,length-2,SEEK_CUR)) return 0;
    }
}
static void saved(ID self, ID cmd, ID image, ID error, void *context) {
    (void)self; (void)cmd; (void)image; (void)context;
    if (error) {
        const char *reason = utf8(m0(error,"localizedDescription"));
        fail(reason ? reason : "Photos did not confirm the save; outcome may be unknown");
    }
    receipt(0);
    /* Photos now owns its own copy. A receipt is sufficient for later retries. */
    unlink(photo_path);
    puts("imported");
    fflush(stdout);
    _exit(0);
}
__attribute__((naked)) void _start(void) {
    __asm__ volatile("ldr r0, [sp]\n\tadd r1, sp, #4\n\tb _main");
}
int main(int argc, char **argv) {
    if (argc != 2 || !component(argv[1])) fail("usage: itphoto staging-id");
    if (getuid() == 0 && (setgid(501) || setuid(501))) fail("cannot become mobile");
    if (getuid() != 501) fail("must run as root or mobile");
    setenv("HOME","/var/mobile",1);
    char folder[224];
    snprintf(folder,sizeof(folder),ROOT "/%s",argv[1]);
    directory(ROOT);
    directory(folder);
    snprintf(photo_path,sizeof(photo_path),"%s/image.jpg",folder);
    snprintf(receipt_path,sizeof(receipt_path),"%s/.photo-receipt",folder);
    int lock = open(ROOT "/.photo-import.lock",O_RDWR|O_CREAT|O_NOFOLLOW,0600);
    if (lock < 0 || flock(lock,LOCK_EX|LOCK_NB)) fail("another photo import is running");
    int fd = open(receipt_path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    if (fd >= 0) {
        char state[16];
        struct stat st;
        if (fstat(fd,&st) || !S_ISREG(st.st_mode)) fail("invalid import receipt");
        ssize_t count = read(fd,state,sizeof(state));
        close(fd);
        if (count == 5 && !memcmp(state,"done\n",5)) complete();
        fail("previous photo import has an uncertain outcome; inspect Saved Photos before importing again");
    }
    if (errno != ENOENT) fail("cannot read import receipt");
    fd = open(photo_path,O_RDONLY|O_NOFOLLOW|O_NONBLOCK);
    struct stat st;
    if (fd < 0 || fstat(fd,&st) || !S_ISREG(st.st_mode) || st.st_size <= 0 || st.st_size > 16*1024*1024)
        fail("staged photo must be a regular JPEG of at most 16 MiB");
    FILE *file = fdopen(fd,"rb");
    if (!file || !jpeg_size(file)) fail("expected an 8-bit baseline JPEG no larger than 2048 pixels per side");
    fclose(file);

    void *objc = dlopen("/usr/lib/libobjc.A.dylib",RTLD_NOW);
    if (!objc) fail("cannot load Objective-C runtime");
    getclass = dlsym(objc,"objc_getClass");
    selector = dlsym(objc,"sel_registerName");
    send = dlsym(objc,"objc_msgSend");
    ID (*allocate)(ID,const char *,unsigned) = dlsym(objc,"objc_allocateClassPair");
    void (*register_class)(ID) = dlsym(objc,"objc_registerClassPair");
    int (*add_method)(ID,ID,void *,const char *) = dlsym(objc,"class_addMethod");
    if (!getclass || !selector || !send || !allocate || !register_class || !add_method ||
        !dlopen("/System/Library/Frameworks/Foundation.framework/Foundation",RTLD_NOW))
        fail("cannot load Foundation");
    m0(m0(getclass("NSAutoreleasePool"),"alloc"),"init");
    ID version = m1(getclass("NSDictionary"),"dictionaryWithContentsOfFile:",
                    string("/System/Library/CoreServices/SystemVersion.plist"));
    const char *build = utf8(m1(version,"objectForKey:",string("ProductBuildVersion")));
    if (!build || strcmp(build,"7E18")) fail("unsupported firmware; expected 7E18");
    void *ui = dlopen("/System/Library/Frameworks/UIKit.framework/UIKit",RTLD_NOW);
    void (*save)(ID,ID,ID,void *) = ui ? dlsym(ui,"UIImageWriteToSavedPhotosAlbum") : NULL;
    if (!save) fail("cannot load the native photo-saving API");
    ID cls = allocate(getclass("NSObject"),"LTPhotoSaver",0);
    if (!cls || !add_method(cls,selector("image:didFinishSavingWithError:contextInfo:"),saved,"v@:@@^v"))
        fail("cannot create photo callback");
    register_class(cls);
    ID target = m0(m0(cls,"alloc"),"init");
    ID image = m1(getclass("UIImage"),"imageWithContentsOfFile:",string(photo_path));
    if (!target || !image) fail("UIKit could not decode the photo");
    receipt(1);  /* Before submitting the mutation, including its async wait. */
    save(image,target,selector("image:didFinishSavingWithError:contextInfo:"),NULL);
    ID deadline = CALL(ID,(ID,ID,double))(getclass("NSDate"),selector("dateWithTimeIntervalSinceNow:"),40.0);
    m1(m0(getclass("NSRunLoop"),"currentRunLoop"),"runUntilDate:",deadline);
    fail("photo save timed out; inspect Saved Photos before importing again");
    return 1;
}
