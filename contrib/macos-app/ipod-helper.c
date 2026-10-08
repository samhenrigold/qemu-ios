/*
 * ipa-chmod for the app, in C so a clean macOS (no python3: /usr/bin/python3 is
 * the Command Line Tools shim) needs nothing beyond libSystem.
 *
 *     ipod-helper ipa-chmod <in.ipa> <out.ipa> <member path>
 */
#include <errno.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

/* stdio can defer a disk-full error until its final buffered write. */
static void finish_output(FILE *stream, const char *path)
{
    if (fclose(stream) != 0) {
        int error = errno;
        unlink(path);
        die("cannot finish %s: %s", path, strerror(error));
    }
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

/* -------------------------------------------------------------- ipa-chmod */
/*
 * An .ipa is a zip, and installd extracts it preserving the archived unix mode.
 * A binary stored 0644 arrives not executable: posix_spawn then fails with
 * EACCES, SpringBoard reports only "exited abnormally with exit status 1", NO
 * crash report is written, and the icon bounces once.
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
    finish_output(f, dst);
    free(buf);
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 5 && strcmp(argv[1], "ipa-chmod") == 0) {
        return ipa_chmod(argv[2], argv[3], argv[4]);
    }
    fprintf(stderr, "usage: ipod-helper ipa-chmod <in.ipa> <out.ipa> <member>\n");
    return 2;
}
