/*
 * The three things the app used to shell out to python3 for.
 *
 * WHY THIS EXISTS
 * ---------------
 * A clean macOS has no python3. /usr/bin/python3 EXISTS, which is what makes
 * this so easy to miss -- but it is one of the ~78 hard links to the Xcode
 * Command Line Tools shim (`stat -f %i /usr/bin/python3 /usr/bin/git /usr/bin/clang`
 * prints the same inode for all of them). On a Mac that has never had the
 * developer tools installed, running it pops "The python3 command requires the
 * command line developer tools" and exits non-zero.
 *
 * The app's FIRST RUN unpacks the NAND, and that was a python3 call. So the app
 * did not fail on some optional feature on a clean Mac -- it failed to start,
 * with a dialog about developer tools that means nothing to the person who just
 * double-clicked an iPod.
 *
 * Everything here is C against libSystem and libz, both of which are in the
 * base OS (/usr/lib/libz.1.dylib), so the bundle gains no new dependency.
 *
 *     ipod-helper nand-unpack <in.itnand> <page dir>
 *     ipod-helper pick-port <preferred>
 *     ipod-helper ipa-chmod <in.ipa> <out.ipa> <member path>
 */
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include <zlib.h>

/* Must match contrib/macos-app/nandpack.py, which still writes these files. */
#define MAGIC "ITNANDP1"
#define PAGE  4160          /* 4096 data + 64 spare */
#define INBUF (1 << 20)
#define OUTBUF (1 << 20)

static int die(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    fputs("ipod-helper: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    exit(1);
}

static uint32_t rd32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint16_t rd16(const unsigned char *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static void wr32(unsigned char *p, uint32_t v)
{
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

/* ------------------------------------------------------------ nand-unpack */
/*
 * The page stream is one zlib stream with no member boundaries -- see
 * nandpack.py for why it is deliberately not an archive format (Apple's notary
 * service opens anything it recognises as one, and NAND pages look like
 * unsigned Mach-O binaries to it). The manifest says which page each
 * PAGE-sized slice belongs to, in order.
 */
struct sink {
    const char *dir;
    const unsigned char *manifest;
    uint32_t count;
    uint32_t done;
    unsigned char pend[PAGE];
    size_t pendlen;
};

static void write_page(struct sink *s, const unsigned char *buf)
{
    char path[4096];
    unsigned cs;
    uint32_t nr;
    FILE *f;

    if (s->done >= s->count) {
        die("more page data than the manifest accounts for");
    }
    /* struct.pack("<BI") -- 1 byte chip select, 4 byte page number, no
     * alignment padding, which is what the '<' in the format string buys. */
    cs = s->manifest[s->done * 5];
    nr = rd32(s->manifest + s->done * 5 + 1);

    snprintf(path, sizeof(path), "%s/cs%u/%u.page", s->dir, cs, nr);
    f = fopen(path, "wb");
    if (!f) {
        die("cannot write %s: %s", path, strerror(errno));
    }
    if (fwrite(buf, 1, PAGE, f) != PAGE) {
        fclose(f);
        die("short write to %s", path);
    }
    fclose(f);
    s->done++;
}

static void feed(struct sink *s, const unsigned char *p, size_t n)
{
    if (s->pendlen) {
        size_t need = PAGE - s->pendlen;
        if (n < need) {
            memcpy(s->pend + s->pendlen, p, n);
            s->pendlen += n;
            return;
        }
        memcpy(s->pend + s->pendlen, p, need);
        write_page(s, s->pend);
        s->pendlen = 0;
        p += need;
        n -= need;
    }
    while (n >= PAGE) {
        write_page(s, p);
        p += PAGE;
        n -= PAGE;
    }
    if (n) {
        memcpy(s->pend, p, n);
        s->pendlen = n;
    }
}

static int nand_unpack(const char *in_path, const char *dir)
{
    unsigned char hdr[16], *manifest = NULL, *mraw = NULL;
    unsigned char *inbuf = NULL, *outbuf = NULL;
    uint32_t count, mlen;
    uLongf mout;
    struct sink s;
    z_stream z;
    FILE *f;
    int rc;
    unsigned cs;

    f = fopen(in_path, "rb");
    if (!f) {
        die("cannot open %s: %s", in_path, strerror(errno));
    }
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, MAGIC, 8) != 0) {
        die("%s is not an ITNANDP1 file", in_path);
    }
    count = rd32(hdr + 8);
    mlen  = rd32(hdr + 12);

    mraw = malloc(mlen);
    if (!mraw || fread(mraw, 1, mlen, f) != mlen) {
        die("%s: truncated manifest", in_path);
    }
    /* 5 bytes per page, and uncompress() needs the exact output size up front,
     * which the count gives us. */
    mout = (uLongf)count * 5;
    manifest = malloc(mout ? mout : 1);
    if (!manifest || uncompress(manifest, &mout, mraw, mlen) != Z_OK ||
        mout != (uLongf)count * 5) {
        die("%s: the manifest does not decompress", in_path);
    }
    free(mraw);

    /* Every chip-select directory the manifest mentions. mkdir -p semantics:
     * an existing directory is success, because an app update unpacks over the
     * top of the previous device rather than deleting it first. */
    for (cs = 0; cs < 256; cs++) {
        char path[4096];
        uint32_t i;
        int want = 0;
        for (i = 0; i < count; i++) {
            if (manifest[i * 5] == cs) { want = 1; break; }
        }
        if (!want) {
            continue;
        }
        snprintf(path, sizeof(path), "%s/cs%u", dir, cs);
        if (mkdir(path, 0755) != 0 && errno != EEXIST) {
            die("cannot create %s: %s", path, strerror(errno));
        }
    }

    memset(&s, 0, sizeof(s));
    s.dir = dir;
    s.manifest = manifest;
    s.count = count;

    memset(&z, 0, sizeof(z));
    if (inflateInit(&z) != Z_OK) {
        die("inflateInit failed");
    }
    inbuf = malloc(INBUF);
    outbuf = malloc(OUTBUF);
    if (!inbuf || !outbuf) {
        die("out of memory");
    }

    for (;;) {
        size_t got = fread(inbuf, 1, INBUF, f);
        if (got == 0) {
            break;
        }
        z.next_in = inbuf;
        z.avail_in = (uInt)got;
        do {
            z.next_out = outbuf;
            z.avail_out = OUTBUF;
            rc = inflate(&z, Z_NO_FLUSH);
            if (rc != Z_OK && rc != Z_STREAM_END && rc != Z_BUF_ERROR) {
                die("%s: the page stream is corrupt (zlib %d)", in_path, rc);
            }
            feed(&s, outbuf, OUTBUF - z.avail_out);
        } while (z.avail_out == 0 && rc != Z_STREAM_END);
        if (rc == Z_STREAM_END) {
            break;
        }
    }
    inflateEnd(&z);
    fclose(f);

    if (s.done != count || s.pendlen != 0) {
        die("wrote %u of %u pages -- the file is truncated", s.done, count);
    }
    printf("unpacked %u pages -> %s\n", s.done, dir);
    free(manifest);
    free(inbuf);
    free(outbuf);
    return 0;
}

/* -------------------------------------------------------------- pick-port */
/*
 * Bind the preferred port to see if it is free, else let the kernel choose.
 * Same TOCTOU as the python it replaces: the socket is closed and the real
 * bind happens later in usbmuxd/QEMU, so two launches racing can still collide.
 */
static int pick_port(const char *pref)
{
    int want = atoi(pref);
    int tries[2], i;

    tries[0] = want;
    tries[1] = 0;

    for (i = 0; i < 2; i++) {
        struct sockaddr_in a;
        socklen_t len = sizeof(a);
        int s = socket(AF_INET, SOCK_STREAM, 0);
        if (s < 0) {
            continue;
        }
        memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        a.sin_port = htons((uint16_t)tries[i]);
        if (bind(s, (struct sockaddr *)&a, sizeof(a)) == 0 &&
            getsockname(s, (struct sockaddr *)&a, &len) == 0) {
            printf("%d\n", ntohs(a.sin_port));
            close(s);
            return 0;
        }
        close(s);
    }
    die("no free port (wanted %d)", want);
    return 1;
}

/* -------------------------------------------------------------- ipa-chmod */
/*
 * An .ipa is a zip, and installd extracts it preserving the archived unix mode.
 * A binary stored 0644 arrives not executable: posix_spawn then fails with
 * EACCES, SpringBoard reports only "exited abnormally with exit status 1", NO
 * crash report is written, and the icon bounces once. See install-ipa.sh.
 *
 * The mode lives in the high 16 bits of the central directory's external
 * attributes field -- it is NOT in the local file header -- so this copies the
 * archive and patches that one 4-byte field in place. That is strictly better
 * than the python it replaces, which rebuilt the whole zip through writestr():
 * here every compressed byte is carried across untouched by construction, which
 * matters because the code directory hashes the file contents.
 */
static int ipa_chmod(const char *src, const char *dst, const char *member)
{
    unsigned char *buf;
    long size;
    size_t i;
    FILE *f;
    long eocd = -1;
    uint32_t cd_off, cd_size;
    uint16_t entries;
    unsigned char *p, *end;
    int patched = 0;

    f = fopen(src, "rb");
    if (!f) {
        die("cannot open %s: %s", src, strerror(errno));
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 22) {
        die("%s is too small to be a zip", src);
    }
    buf = malloc((size_t)size);
    if (!buf || fread(buf, 1, (size_t)size, f) != (size_t)size) {
        die("cannot read %s", src);
    }
    fclose(f);

    /* End of central directory, scanned backwards: the record is 22 bytes plus
     * a comment of up to 65535. */
    for (i = (size_t)size - 22 + 1; i-- > 0;) {
        if (rd32(buf + i) == 0x06054b50) {
            eocd = (long)i;
            break;
        }
        if ((size_t)size - i > 22 + 65535) {
            break;
        }
    }
    if (eocd < 0) {
        die("%s has no zip end-of-central-directory record", src);
    }
    entries = rd16(buf + eocd + 10);
    cd_size = rd32(buf + eocd + 12);
    cd_off  = rd32(buf + eocd + 16);
    if (cd_off == 0xffffffffu || cd_size == 0xffffffffu) {
        die("%s is a zip64 archive, which this does not handle", src);
    }
    if ((size_t)cd_off + cd_size > (size_t)size) {
        die("%s: the central directory runs past the end of the file", src);
    }

    p = buf + cd_off;
    end = buf + cd_off + cd_size;
    while (p + 46 <= end && rd32(p) == 0x02014b50) {
        uint16_t nlen = rd16(p + 28);
        uint16_t elen = rd16(p + 30);
        uint16_t clen = rd16(p + 32);
        if (p + 46 + nlen > end) {
            break;
        }
        if (nlen == strlen(member) &&
            memcmp(p + 46, member, nlen) == 0) {
            /* Keep the file-type bits and the low 16 bits (DOS attributes)
             * exactly as they were; only the permission word changes. */
            uint32_t ext = rd32(p + 38);
            wr32(p + 38, (0100755u << 16) | (ext & 0xffffu));
            patched = 1;
            break;
        }
        p += 46 + nlen + elen + clen;
        (void)entries;
    }

    if (!patched) {
        die("%s is not in %s", member, src);
    }

    f = fopen(dst, "wb");
    if (!f) {
        die("cannot write %s: %s", dst, strerror(errno));
    }
    if (fwrite(buf, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        die("short write to %s", dst);
    }
    fclose(f);
    free(buf);
    return 0;
}

/* ------------------------------------------------------------- macho-info */
/*
 * The two things install-ipa.sh needs to know about an app binary, without
 * otool. `command -v otool` is NOT a usable guard: /usr/bin/otool is the SAME
 * Command Line Tools shim as /usr/bin/python3 (same inode, same 118640 bytes),
 * so on a clean Mac the guard PASSES and both otool pipelines then return
 * nothing. That silently sets LINKS_GLES=0 for a GL app, the engine
 * replacement is never installed, and the device wedges on first launch with
 * no warning printed anywhere.
 *
 * Prints "cryptid=N" and "gles=0|1". A file it cannot parse is not an error --
 * it prints nothing and exits 1, and the caller keeps its conservative
 * default.
 */
#define LC_LOAD_DYLIB       0x0c
#define LC_ENCRYPTION_INFO  0x21

static void scan_thin(const unsigned char *b, size_t n, size_t off,
                      int *cryptid, int *gles)
{
    uint32_t ncmds, i, p;

    if (off + 28 > n || rd32(b + off) != 0xfeedfaceu) {
        return;         /* not a 32-bit little-endian Mach-O */
    }
    ncmds = rd32(b + off + 16);
    p = (uint32_t)off + 28;

    for (i = 0; i < ncmds; i++) {
        uint32_t cmd, sz;
        if ((size_t)p + 8 > n) {
            return;
        }
        cmd = rd32(b + p);
        sz  = rd32(b + p + 4);
        if (sz < 8 || (size_t)p + sz > n) {
            return;
        }
        if (cmd == LC_ENCRYPTION_INFO && sz >= 20) {
            *cryptid = (int)rd32(b + p + 16);
        } else if (cmd == LC_LOAD_DYLIB && sz >= 24) {
            uint32_t no = rd32(b + p + 8);
            if (no < sz) {
                const char *name = (const char *)(b + p + no);
                size_t max = sz - no, k;
                for (k = 0; k < max && name[k]; k++) {
                    ;
                }
                if (k < max && strstr(name, "OpenGLES")) {
                    *gles = 1;
                }
            }
        }
        p += sz;
    }
}

static int macho_info(const char *path)
{
    unsigned char *b;
    long size;
    FILE *f;
    int cryptid = 0, gles = 0;

    f = fopen(path, "rb");
    if (!f) {
        return 1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size < 28) {
        fclose(f);
        return 1;
    }
    b = malloc((size_t)size);
    if (!b || fread(b, 1, (size_t)size, f) != (size_t)size) {
        fclose(f);
        return 1;
    }
    fclose(f);

    /* FAT headers are big-endian by definition; thin armv6 is little. */
    if (size >= 8 && b[0] == 0xca && b[1] == 0xfe &&
        b[2] == 0xba && b[3] == 0xbe) {
        uint32_t nfat = ((uint32_t)b[4] << 24) | ((uint32_t)b[5] << 16) |
                        ((uint32_t)b[6] << 8) | b[7];
        uint32_t i;
        for (i = 0; i < nfat && 8 + (i + 1) * 20 <= (uint32_t)size; i++) {
            const unsigned char *a = b + 8 + i * 20;
            uint32_t o = ((uint32_t)a[8] << 24) | ((uint32_t)a[9] << 16) |
                         ((uint32_t)a[10] << 8) | a[11];
            scan_thin(b, (size_t)size, o, &cryptid, &gles);
        }
    } else {
        scan_thin(b, (size_t)size, 0, &cryptid, &gles);
    }

    printf("cryptid=%d\ngles=%d\n", cryptid, gles);
    free(b);
    return 0;
}

/* ------------------------------------------------------------------- blob */
/*
 * An opaque container for the armv6 iOS binaries the app ships (the MBX GL
 * engine replacement, and the home-screen placeholder tool).
 *
 * They cannot go in the bundle as ordinary files. Apple's notary service walks
 * every file looking for Mach-O binaries and rejects unsigned executables
 * without a hardened runtime -- and these ARE Mach-O, just for a CPU no Mac
 * has run in fifteen years. They also cannot be signed for macOS. This is the
 * same problem the NAND had, and the same answer: one file, a custom magic and
 * a single zlib stream, with nothing in it that announces a container of files.
 *
 *     blob-pack <out> <name>=<path> ...
 *     blob-unpack <in> <dir>
 *
 * Entries are name-length, name, mode, length, bytes -- concatenated, then the
 * whole thing compressed as one stream.
 */
#define BLOB_MAGIC "ITBLOB01"

static int blob_pack(int argc, char **argv)
{
    unsigned char *raw = NULL, *comp = NULL;
    size_t rawlen = 0, cap = 0;
    uLongf clen;
    FILE *out;
    int i;

    for (i = 3; i < argc; i++) {
        char *eq = strchr(argv[i], '=');
        const char *name, *path;
        struct stat st;
        FILE *f;
        size_t nl;

        if (!eq) {
            die("expected <name>=<path>, got %s", argv[i]);
        }
        *eq = '\0';
        name = argv[i];
        path = eq + 1;
        if (stat(path, &st) != 0) {
            die("cannot stat %s: %s", path, strerror(errno));
        }
        nl = strlen(name);

        while (rawlen + nl + 16 + (size_t)st.st_size > cap) {
            cap = cap ? cap * 2 : (1 << 20);
            raw = realloc(raw, cap);
            if (!raw) {
                die("out of memory");
            }
        }
        raw[rawlen++] = (unsigned char)(nl & 0xff);
        raw[rawlen++] = (unsigned char)((nl >> 8) & 0xff);
        memcpy(raw + rawlen, name, nl);
        rawlen += nl;
        wr32(raw + rawlen, (uint32_t)(st.st_mode & 07777));
        rawlen += 4;
        wr32(raw + rawlen, (uint32_t)st.st_size);
        rawlen += 4;

        f = fopen(path, "rb");
        if (!f || fread(raw + rawlen, 1, (size_t)st.st_size, f) !=
                (size_t)st.st_size) {
            die("cannot read %s", path);
        }
        fclose(f);
        rawlen += (size_t)st.st_size;
    }
    if (!rawlen) {
        die("nothing to pack");
    }

    clen = compressBound(rawlen);
    comp = malloc(clen);
    if (!comp || compress2(comp, &clen, raw, rawlen, 9) != Z_OK) {
        die("compression failed");
    }
    out = fopen(argv[2], "wb");
    if (!out) {
        die("cannot write %s: %s", argv[2], strerror(errno));
    }
    {
        unsigned char hdr[16];
        memcpy(hdr, BLOB_MAGIC, 8);
        wr32(hdr + 8, (uint32_t)rawlen);
        wr32(hdr + 12, (uint32_t)clen);
        fwrite(hdr, 1, 16, out);
    }
    fwrite(comp, 1, clen, out);
    fclose(out);
    printf("packed %d file(s) -> %s\n", argc - 3, argv[2]);
    free(raw);
    free(comp);
    return 0;
}

static int blob_unpack(const char *in, const char *dir)
{
    unsigned char hdr[16], *comp, *raw;
    uint32_t rawlen, clen;
    uLongf outlen;
    size_t p = 0;
    FILE *f;

    f = fopen(in, "rb");
    if (!f) {
        die("cannot open %s: %s", in, strerror(errno));
    }
    if (fread(hdr, 1, 16, f) != 16 || memcmp(hdr, BLOB_MAGIC, 8) != 0) {
        die("%s is not an ITBLOB01 file", in);
    }
    rawlen = rd32(hdr + 8);
    clen   = rd32(hdr + 12);
    comp = malloc(clen);
    raw  = malloc(rawlen);
    if (!comp || !raw || fread(comp, 1, clen, f) != clen) {
        die("%s is truncated", in);
    }
    fclose(f);
    outlen = rawlen;
    if (uncompress(raw, &outlen, comp, clen) != Z_OK || outlen != rawlen) {
        die("%s does not decompress", in);
    }
    free(comp);

    if (mkdir(dir, 0755) != 0 && errno != EEXIST) {
        die("cannot create %s: %s", dir, strerror(errno));
    }
    while (p + 2 <= rawlen) {
        char path[4096], name[1024];
        uint32_t mode, len;
        size_t nl = (size_t)raw[p] | ((size_t)raw[p + 1] << 8);
        FILE *o;

        p += 2;
        if (nl >= sizeof(name) || p + nl + 8 > rawlen) {
            die("%s is malformed", in);
        }
        memcpy(name, raw + p, nl);
        name[nl] = '\0';
        p += nl;
        mode = rd32(raw + p); p += 4;
        len  = rd32(raw + p); p += 4;
        if (p + len > rawlen || strchr(name, '/')) {
            die("%s is malformed", in);
        }
        snprintf(path, sizeof(path), "%s/%s", dir, name);
        o = fopen(path, "wb");
        if (!o || fwrite(raw + p, 1, len, o) != len) {
            die("cannot write %s", path);
        }
        fclose(o);
        chmod(path, (mode_t)mode);
        p += len;
    }
    free(raw);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc >= 2 && strcmp(argv[1], "macho-info") == 0 && argc == 3) {
        return macho_info(argv[2]);
    }
    if (argc >= 4 && strcmp(argv[1], "blob-pack") == 0) {
        return blob_pack(argc, argv);
    }
    if (argc >= 2 && strcmp(argv[1], "blob-unpack") == 0 && argc == 4) {
        return blob_unpack(argv[2], argv[3]);
    }
    if (argc >= 2 && strcmp(argv[1], "nand-unpack") == 0 && argc == 4) {
        return nand_unpack(argv[2], argv[3]);
    }
    if (argc >= 2 && strcmp(argv[1], "pick-port") == 0 && argc == 3) {
        return pick_port(argv[2]);
    }
    if (argc >= 2 && strcmp(argv[1], "ipa-chmod") == 0 && argc == 5) {
        return ipa_chmod(argv[2], argv[3], argv[4]);
    }
    fprintf(stderr,
            "usage: ipod-helper nand-unpack <in.itnand> <page dir>\n"
            "       ipod-helper pick-port <preferred>\n"
            "       ipod-helper ipa-chmod <in.ipa> <out.ipa> <member>\n"
            "       ipod-helper macho-info <binary>\n"
            "       ipod-helper blob-pack <out> <name>=<path> ...\n"
            "       ipod-helper blob-unpack <in> <dir>\n");
    return 2;
}
