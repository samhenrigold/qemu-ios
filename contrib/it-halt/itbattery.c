/* Read-only guest IOPMPowerSource properties for native power regressions. */
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int write(int, const void *, unsigned);
extern void _exit(int) __attribute__((noreturn));
__attribute__((naked)) void _start(void)
{
    __asm__ volatile("ldr r0, [sp]\n\tadd r1, sp, #4\n\tb _main");
}
int main(int argc, char **argv)
{
    void *io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", 2);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 2);
    if (!io || !cf) _exit(2);
    void *(*match)(const char *) = dlsym(io, "IOServiceMatching");
    unsigned (*get)(unsigned, void *) = dlsym(io, "IOServiceGetMatchingService");
    int (*props)(unsigned, void **, void *, unsigned) = dlsym(io, "IORegistryEntryCreateCFProperties");
    void *(*xml)(void *, void *) = dlsym(cf, "CFPropertyListCreateXMLData");
    const char *(*bytes)(void *) = dlsym(cf, "CFDataGetBytePtr");
    int (*length)(void *) = dlsym(cf, "CFDataGetLength");
    if (!match || !get || !props || !xml || !bytes || !length) _exit(2);
    unsigned service = get(0, match("IOPMPowerSource"));
    void *properties = 0;
    if (!service || props(service, &properties, 0, 0) || !properties) _exit(3);
    void *data = xml(0, properties);
    if (!data) _exit(4);
    int size = length(data);
    if (size <= 0 || size > 1048576) _exit(4);
    const char *cursor = bytes(data);
    while (size) {
        int count = write(1, cursor, size);
        if (count <= 0) _exit(5);
        cursor += count;
        size -= count;
    }
    _exit(0);
}
