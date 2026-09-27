/*
 * it_cctest -- CommonCrypto known-answer run on the guest CPU.
 *
 * Checks SHA-1, HMAC-SHA1 and AES-128-CBC over buffers from 1 B to 64 KiB at
 * every byte alignment, one-shot and in TLS-like pieces, against answers the
 * host computes from the same xorshift data (tests/ipad1/cctest.py). One line
 * per case on stdout: "op len align fnv32-of-output". A wrong line means the
 * emulated CPU (or a hardware-crypto model) computes a different answer than
 * real ARM would.
 */
extern void *dlopen(const char *, int);
extern void *dlsym(void *, const char *);
extern int printf(const char *, ...);
extern int fflush(void *);
extern void *malloc(unsigned long);
extern unsigned int sleep(unsigned int);
extern void *memcpy(void *, const void *, unsigned long);

typedef unsigned int u32;
typedef unsigned char u8;

static u32 seed;
static u32 rnd(void) { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; }
static void fill(u8 *p, u32 n, u32 s) { seed = s; for (u32 i = 0; i < n; i++) p[i] = (u8)rnd(); }
static u32 fnv(const u8 *p, u32 n) { u32 h = 2166136261u; for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }

static u8 *(*sha1)(const void *, u32, u8 *);
static void (*hmac)(u32, const void *, u32, const void *, u32, void *);
static void (*hinit)(void *, u32, const void *, u32);
static void (*hupd)(void *, const void *, u32);
static void (*hfin)(void *, void *);
static int (*crypt)(u32, u32, u32, const void *, u32, const void *, const void *, u32, void *, u32, u32 *);
static int (*ccreate)(u32, u32, u32, const void *, u32, const void *, void **);
static int (*cupd)(void *, const void *, u32, void *, u32, u32 *);
static int (*crel)(void *);

static const u32 lens[] = { 1, 13, 15, 16, 17, 63, 64, 65, 100, 511, 1023, 1024, 1025, 1440,
                            1460, 1500, 2048, 4095, 4096, 4097, 8191, 16384, 16385, 32768, 65536 };

int main(void)
{
    void *h = dlopen("/usr/lib/libSystem.B.dylib", 2);
    u8 *src = malloc(65536 + 64), *out = malloc(65536 + 64), *back = malloc(65536 + 64);
    u8 key[20], iv[16], md[20], ctx[512];
    u32 moved;

    sha1 = dlsym(h, "CC_SHA1"); hmac = dlsym(h, "CCHmac");
    hinit = dlsym(h, "CCHmacInit"); hupd = dlsym(h, "CCHmacUpdate"); hfin = dlsym(h, "CCHmacFinal");
    crypt = dlsym(h, "CCCrypt"); ccreate = dlsym(h, "CCCryptorCreate");
    cupd = dlsym(h, "CCCryptorUpdate"); crel = dlsym(h, "CCCryptorRelease");
    sleep(20);                                  /* let the boot's own work settle */
    printf("it_cctest: start %d\n", sha1 && hmac && crypt && ccreate ? 1 : 0);
    fill(key, 20, 7); fill(iv, 16, 9);
    for (u32 li = 0; li < sizeof(lens) / sizeof(lens[0]); li++) {
        u32 n = lens[li], a16 = n & ~15u;
        for (u32 al = 0; al < 4; al++) {
            u8 *d = src + al, *o = out + al;
            fill(d, n, 1000 + n + al);
            sha1(d, n, md);
            printf("sha1 %u %u %08x\n", n, al, fnv(md, 20));
            hmac(0, key, 20, d, n, md);
            printf("hmac %u %u %08x\n", n, al, fnv(md, 20));
            /* TLS 1.0 MAC shape: 13-byte header then the payload */
            hinit(ctx, 0, key, 20); hupd(ctx, d, n < 13 ? n : 13);
            if (n > 13) hupd(ctx, d + 13, n - 13);
            hfin(ctx, md);
            printf("hmacp %u %u %08x\n", n, al, fnv(md, 20));
            if (!a16) continue;
            memcpy(back + (3 - al), d, n);
            printf("memcpy %u %u %08x\n", n, al, fnv(back + (3 - al), n));
            crypt(0, 0, 0, key, 16, iv, d, a16, o, a16 + 16, &moved);
            printf("aesenc %u %u %08x %u\n", a16, al, fnv(o, a16), moved);
            if (a16 >= 4096) {
                printf("aesenc-chunks %u %u", a16, al);
                for (u32 c = 0; c < a16; c += 1024) printf(" %08x", fnv(o + c, 1024));
                printf(" tail");
                for (u32 i = 4064; i < 4112 && i < a16; i++) printf("%02x", o[i]);
                printf("\n");
            }
            crypt(1, 0, 0, key, 16, iv, o, a16, back, a16 + 16, &moved);
            printf("aesdec %u %u %08x %u\n", a16, al, fnv(back, a16), moved);
            /* SecureTransport shape: one cryptor, updates of up to 16 KiB */
            void *ref = 0;
            ccreate(1, 0, 0, key, 16, iv, &ref);
            for (u32 off = 0; off < a16; off += 16384) {
                u32 m = a16 - off < 16384 ? a16 - off : 16384;
                cupd(ref, o + off, m, back + off, m, &moved);
            }
            crel(ref);
            printf("aesdecs %u %u %08x\n", a16, al, fnv(back, a16));
        }
        fflush(0);
    }
    printf("it_cctest: done\n");
    fflush(0);
    return 0;
}
