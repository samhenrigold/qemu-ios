extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern long write(int,const void*,unsigned long);
extern void _exit(int);
extern int fflush(void*);
extern int printf(const char*,...);
__attribute__((naked)) void _start(void) { __asm__ volatile("ldr r0,[sp]\n\tadd r1,sp,#4\n\tb _main"); }
int main(int argc,char **argv) {
 if(argc!=2)_exit(2);
 dlopen("/System/Library/Frameworks/Foundation.framework/Foundation",2);
 void *rt=dlopen("/usr/lib/libobjc.A.dylib",2);
 void *(*cls)(const char*)=dlsym(rt,"objc_getClass");
 void *(*sel)(const char*)=dlsym(rt,"sel_registerName");
 void *(*msg)(void*,void*,...)=dlsym(rt,"objc_msgSend");
 void *pool=msg(msg(cls("NSAutoreleasePool"),sel("alloc")),sel("init"));
 void *str=msg(cls("NSString"),sel("stringWithUTF8String:"),argv[1]);
 void *url=msg(cls("NSURL"),sel("URLWithString:"),str);
 void *req=msg(cls("NSURLRequest"),sel("requestWithURL:cachePolicy:timeoutInterval:"),url,1,30.0);
 void *resp=0,*err=0;
 void *data=msg(cls("NSURLConnection"),sel("sendSynchronousRequest:returningResponse:error:"),req,&resp,&err);
 if(!data){const char *error=msg(msg(err,sel("description")),sel("UTF8String"));printf("ERROR %s\n",error);fflush(0);_exit(1);}
 printf("HTTP %ld\n",(long)msg(resp,sel("statusCode")));fflush(0);
 write(1,msg(data,sel("bytes")),(unsigned long)msg(data,sel("length")));
 msg(pool,sel("drain"));_exit(0);return 0;
}
