/* Guest-only trust-store management for Light Touch's local TLS bridge.
 * Uses securityd's native API; never edits the trust database behind its back. */
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern void *fopen(const char *, const char *);
extern unsigned long fread(void *, unsigned long, unsigned long, void *);
extern int fclose(void *);
extern int strcmp(const char *, const char *);
extern int printf(const char *, ...);
extern int fflush(void *);
extern void _exit(int);
__attribute__((naked)) void _start(void) { __asm__ volatile("ldr r0,[sp]\n\tadd r1,sp,#4\n\tb _main"); }
int main(int argc, char **argv)
{
    int remove = argc == 3 && !strcmp(argv[1], "remove");
    if (argc != 3 || (!remove && strcmp(argv[1], "add"))) _exit(2);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 2);
    void *sec = dlopen("/System/Library/Frameworks/Security.framework/Security", 2);
    void *(*data_create)(void *, const unsigned char *, long) = dlsym(cf, "CFDataCreate");
    void (*release)(void *) = dlsym(cf, "CFRelease");
    void *(*cert_create)(void *, void *) = dlsym(sec, "SecCertificateCreateWithData");
    void *(*store_for_domain)(int) = dlsym(sec, "SecTrustStoreForDomain");
    int (*set_trust)(void *, void *, void *) = dlsym(sec, "SecTrustStoreSetTrustSettings");
    int (*remove_cert)(void *, void *) = dlsym(sec, "SecTrustStoreRemoveCertificate");
    if (!data_create || !release || !cert_create || !store_for_domain || !set_trust || !remove_cert) _exit(3);
    unsigned char bytes[65537];
    void *file = fopen(argv[2], "rb");
    if (!file) _exit(4);
    unsigned long count = fread(bytes, 1, sizeof(bytes), file);
    fclose(file);
    if (!count || count == sizeof(bytes)) _exit(4);
    void *data = data_create(0, bytes, count);
    void *cert = data ? cert_create(0, data) : 0;
    void *store = store_for_domain(2); /* user trust domain, private to the guest */
    int status = !cert || !store ? -50 : remove ? remove_cert(store, cert) : set_trust(store, cert, 0);
    if (cert) release(cert);
    if (data) release(data);
    printf("Guest trust %s: %d\n", remove ? "remove" : "add", status);
    fflush(0);
    _exit(status ? 1 : 0);
    return 0;
}
