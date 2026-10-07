/* Main-run-loop typing and UI inspection for iPhone OS 3.1.3.
 * The constructor only starts a worker; CF/ObjC setup happens after dyld has
 * finished. SpringBoard-spawned apps inherit this dylib (native probe verified).
 */
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);

typedef void *ID;
static ID (*getClass)(const char *);
static ID (*sel)(const char *);
static ID (*msg)(ID, ID, ...);
static void (*releaseCF)(ID);
static ID (*createBytes)(ID,const uint8_t *,long,unsigned,unsigned char);
static ID (*frontmost)(void);
static unsigned char (*processID)(ID,int *);
static ID (*timerCreate)(ID,double,double,unsigned,long,void (*)(ID,void *),void *);
static void (*addTimer)(ID,ID,ID);
static ID (*mainLoop)(void);
static double (*currentTime)(void);
static int pid, springboard;
static pthread_mutex_t focus_lock = PTHREAD_MUTEX_INITIALIZER;
static int cached_foreground;

static int current_foreground(void)
{
    pthread_mutex_lock(&focus_lock);
    int value = cached_foreground;
    pthread_mutex_unlock(&focus_lock);
    return value;
}
static unsigned (*serverPort)(void);
static int (*screenLock)(unsigned,unsigned char *,unsigned char *);
static uint64_t cookie;
static char request[65536 + 4096];
static char output[256 * 1024];
static unsigned output_len;
static unsigned tree_nodes;

static int64_t call(unsigned op, void *buffer, unsigned offset, unsigned length)
{
    struct __attribute__((packed)) {
        uint32_t op, address, offset, length;
        uint64_t token;
        uint8_t pad[12];
        int64_t result,error;
    } q;
    memset(&q,0,sizeof(q));
    q.op=op;q.address=(uint32_t)buffer;q.offset=offset;q.length=length;q.token=(op>=0x167 && op<=0x169) ? cookie : 0;
    __asm__ volatile("mcr p15, 3, %0, c15, c15, 0" : : "r"(&q) : "memory");
    return q.result;
}

static int responds(ID object,const char *selector)
{
    return (unsigned)msg(object,sel("respondsToSelector:"),sel(selector)) != 0;
}

static void append(const char *text)
{
    if (!text) return;
    unsigned n=strlen(text);
    if(n>sizeof(output)-output_len)n=sizeof(output)-output_len;
    memcpy(output+output_len,text,n);output_len+=n;
}

static void dump_view(ID view,unsigned depth)
{
    if(!view || depth>48 || tree_nodes++>=2048 || output_len==sizeof(output))return;
    if(responds(view,"isSecureTextEntry") && (unsigned)msg(view,sel("isSecureTextEntry"))) {
        append("<secure text entry>\n");
        return; /* Descriptions and field-editor children can expose the value. */
    }
    ID description=msg(view,sel("description"));
    append((const char *)msg(description,sel("UTF8String")));append("\n");
    if(responds(view,"text")) {
        ID text=msg(view,sel("text"));
        if(text && responds(text,"UTF8String")) {
            append("text: ");append((const char *)msg(text,sel("UTF8String")));append("\n");
        }
    }
    ID children=msg(view,sel("subviews"));
    unsigned count=(unsigned)msg(children,sel("count"));
    if(count>2048)count=2048;
    for(unsigned i=0;i<count;i++)dump_view(msg(children,sel("objectAtIndex:"),i),depth+1);
}

static void finish(int status)
{
    unsigned off=0;
    do {
        unsigned n=output_len-off;if(n>1024)n=1024;
        if(call(0x168,output+off,off,n)!=n)return;
        off+=n;
    }while(off<output_len);
    call(0x169,0,status,0);
}

/* 0 background, 1 active input target, 2 locked SpringBoard (inspection only). */
static int foreground_state(void)
{
    unsigned char locked=0,passcode=0;
    if(screenLock(serverPort(),&locked,&passcode))return 0;
    if(locked)return springboard ? 2 : 0;
    ID identifier=frontmost();
    if(!identifier)return springboard;
    int front=0;
    if(identifier) {
        processID(identifier,&front);
        releaseCF(identifier);
    }
    return front==pid;
}

static void tick(ID timer,void *unused)
{
    ID pool=msg(msg(getClass("NSAutoreleasePool"),sel("alloc")),sel("init"));
    ID app=msg(getClass("UIApplication"),sel("sharedApplication"));
    if(!app)goto out;
    int64_t pending=call(0x166,0,springboard ? 0 : pid,0);
    if(pending>0 && current_foreground()) {
        cookie=pending;
        /* Debug writes cannot allocate guest VM pages. */
        memset(request,0,sizeof(request));
        unsigned length=0;
        int status=0;
        for(;;) {
            if(length+1024>sizeof(request)){status=-EFBIG;break;}
            int64_t n=call(0x167,request+length,length,1024);
            if(n<0){status=-EIO;break;}
            length+=n;
            if(n<1024)break;
        }
        output_len=0;
        char *nl=memchr(request,'\n',length);
        char *op=nl?memchr(request,' ',nl-request):0;
        if(!op)status=-EINVAL;
        int foreground=current_foreground();
        if(!status && !foreground)status=-EAGAIN;
        if(!status) {
            *nl=0;op++;
            char *space=strchr(op,' ');if(space)*space=0;
            ID keyboard=msg(getClass("UIKeyboardImpl"),sel("activeInstance"));
            if(!strcmp(op,"uidump")) {
                tree_nodes=0;dump_view(msg(app,sel("keyWindow")),0);
            }else if(foreground==2)status=-EACCES;
            else if(!keyboard || !msg(keyboard,sel("delegate")))status=-ENODEV;
            else if(!strcmp(op,"backspace")) {
                if(responds(keyboard,"deleteFromInput"))msg(keyboard,sel("deleteFromInput"));
                else status=-ENOSYS;
            }else if(!strcmp(op,"type")) {
                ID text=createBytes(0,(uint8_t *)nl+1,length-(nl+1-request),0x08000100,0);
                if(!text)status=-EINVAL;
                else {
                    /* Bulk text is a text-input operation. Feeding an entire
                     * paragraph as one keyboard key creates a giant correction
                     * candidate on 3.1.3. These delegates implement insertText:. */
                    ID delegate=msg(keyboard,sel("delegate"));
                    if(responds(delegate,"insertText:"))msg(delegate,sel("insertText:"),text);
                    else status=-ENOSYS;
                    releaseCF(text);
                }
            }else status=-ENOSYS;
        }
        finish(status);
    }
    /* The foreground process consumes keys, discarding them without a keyboard.
     * Check suspension first to avoid an IPC round trip for background apps. */
    if(!(unsigned)msg(app,sel("isSuspended")) && call(0x131,0,0,0)>0) {
        ID keyboard=msg(getClass("UIKeyboardImpl"),sel("activeInstance"));
        int foreground=current_foreground();
        if(foreground) {
            int accepts=foreground==1 && keyboard && msg(keyboard,sel("delegate"));
            for(unsigned i=0;i<32;i++) {
                int64_t ch=call(0x130,0,0,0);
                if(ch<=0)break;
                if(!accepts)continue; /* Unfocused keystrokes must not leak into a later field. */
                if(ch==8)msg(keyboard,sel("deleteFromInput"));
                else {
                    uint16_t value=ch;
                    ID (*createChars)(ID,const uint16_t *,long);
                    static void *cf;
                    if(!cf)cf=dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",2);
                    createChars=dlsym(cf,"CFStringCreateWithCharacters");
                    ID text=createChars(0,&value,1);
                    if(text){msg(keyboard,sel("addInputString:"),text);releaseCF(text);}
                }
            }
        }
    }
out:
    msg(pool,sel("release"));
}

static void *setup(void *unused)
{
    sleep(2);
    void *objc=dlopen("/usr/lib/libobjc.A.dylib",2);
    if(!objc)return 0;
    getClass=dlsym(objc,"objc_getClass");sel=dlsym(objc,"sel_registerName");msg=dlsym(objc,"objc_msgSend");
    if(!getClass||!sel||!msg||!getClass("UIApplication"))return 0;
    void *cf=dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation",2);
    void *sbs=dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices",2);
    if(!cf||!sbs)return 0;
    releaseCF=dlsym(cf,"CFRelease");createBytes=dlsym(cf,"CFStringCreateWithBytes");
    timerCreate=dlsym(cf,"CFRunLoopTimerCreate");addTimer=dlsym(cf,"CFRunLoopAddTimer");
    mainLoop=dlsym(cf,"CFRunLoopGetMain");currentTime=dlsym(cf,"CFAbsoluteTimeGetCurrent");
    frontmost=dlsym(sbs,"SBSCopyFrontmostApplicationDisplayIdentifier");
    processID=dlsym(sbs,"SBSProcessIDForDisplayIdentifier");
    serverPort=dlsym(sbs,"SBSSpringBoardServerPort");screenLock=dlsym(sbs,"SBGetScreenLockStatus");
    ID *modes=dlsym(cf,"kCFRunLoopCommonModes");
    if(!releaseCF||!createBytes||!timerCreate||!addTimer||!mainLoop||!currentTime||!frontmost||!processID||!serverPort||!screenLock||!modes)return 0;
    pid=getpid();
    /* SpringBoard needs its measured cold-launch grace period. Ordinary apps
     * can start once their run loop is live; they must not inherit that delay. */
    ID (*bundle)(void)=dlsym(cf,"CFBundleGetMainBundle");
    ID (*identifier)(ID)=dlsym(cf,"CFBundleGetIdentifier");
    unsigned char (*getString)(ID,char *,long,unsigned)=dlsym(cf,"CFStringGetCString");
    char name[256]={0};
    double delay=12;
    if(bundle && identifier && getString) {
        ID value=identifier(bundle());
        if(value && getString(value,name,sizeof(name),0x08000100)) {
            springboard=!strcmp(name,"com.apple.springboard");
            if(!springboard)delay=0.25;
        }
    }
    ID timer=timerCreate(0,currentTime()+delay,0.05,0,0,tick,0);
    if(timer){addTimer(mainLoop(),timer,*modes);releaseCF(timer);}
    /* SBS performs synchronous Mach IPC. Never call it on SpringBoard's
     * main thread: that same thread serves the request. The worker publishes
     * focus only while input or a routed UI request needs it. */
    for (;;) {
        int foreground = 0;
        if (call(0x131,0,0,0)>0 || call(0x166,0,springboard ? 0 : pid,0)>0)
            foreground = foreground_state();
        pthread_mutex_lock(&focus_lock);
        cached_foreground = foreground;
        pthread_mutex_unlock(&focus_lock);
        usleep(25000);
    }
    return 0;
}

__attribute__((constructor)) static void inserted(void)
{
    pthread_t thread;
    if(!pthread_create(&thread,0,setup,0))pthread_detach(thread);
}
