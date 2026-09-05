/* Read SpringBoard's actual foreground app and localized display name.
 * Standalone read-only helper: does not share sblaunch's command file. */
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern long write(int, const void *, unsigned long);
extern void _exit(int);
static void out(const char *s) {
    unsigned n = 0;
    while (s[n]) n++;
    write(1, s, n);
    write(1, "\n", 1);
}
int main(void) {
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 2);
    void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", 2);
    void *(*frontmost)(void) = dlsym(sbs, "SBSCopyFrontmostApplicationDisplayIdentifier");
    void *(*name)(void *) = dlsym(sbs, "SBSCopyLocalizedApplicationNameForDisplayIdentifier");
    unsigned char (*string)(void *, char *, long, unsigned) = dlsym(cf, "CFStringGetCString");
    void (*release)(void *) = dlsym(cf, "CFRelease");
    if (!frontmost || !name || !string || !release) _exit(1);
    void *identifier = frontmost();
    if (!identifier) { out("Home Screen"); _exit(0); }
    void *title = name(identifier);
    char buffer[1024];
    if (!string(title ? title : identifier, buffer, sizeof(buffer), 0x08000100)) _exit(1);
    out(buffer);
    if (title) release(title);
    release(identifier);
    _exit(0);
    return 0;
}
