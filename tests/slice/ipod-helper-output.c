/* Host helper output must fail on deferred disk errors, without guest firmware.
 *
 * ipa-chmod sets the member's mode in the central directory; when the OS reports a deferred close error
 * (ENOSPC injected at fclose, without exhausting real disk) it fails with "cannot finish" and leaves no output.
 *
 * SLICE contrib/macos-app/ipod-helper.c fn die finish_output rd32 rd16 wr32 ipa_chmod
 * CFLAGS -lz
 */
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <zlib.h>

static bool full_disk;
static int test_fclose(FILE *stream)
{
    int writing = (fcntl(fileno(stream), F_GETFL) & O_ACCMODE) != O_RDONLY;
    int result = fclose(stream);
    if (full_disk && writing) { errno = ENOSPC; return EOF; }
    return result;
}
#define fclose test_fclose
#include "slice.h"
#undef fclose

static void put16(FILE *f, unsigned v) { fputc(v & 0xff, f); fputc(v >> 8 & 0xff, f); }
static void put32(FILE *f, uint32_t v) { put16(f, v & 0xffff); put16(f, v >> 16); }

/* A one-member stored zip, as Python's zipfile.writestr makes it (mode 0600 regular file). */
static void write_zip(const char *path, const char *member, const char *data)
{
    FILE *f = fopen(path, "wb");
    assert(f);
    uint32_t n = strlen(member), len = strlen(data), crc = crc32(0, (const Bytef *)data, len);
    put32(f, 0x04034b50); put16(f, 20); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0x21);
    put32(f, crc); put32(f, len); put32(f, len); put16(f, n); put16(f, 0);
    fwrite(member, 1, n, f); fwrite(data, 1, len, f);
    uint32_t cd = ftell(f);
    put32(f, 0x02014b50); put16(f, 0x0314); put16(f, 20); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0x21);
    put32(f, crc); put32(f, len); put32(f, len); put16(f, n); put16(f, 0); put16(f, 0); put16(f, 0); put16(f, 0);
    put32(f, 0100600u << 16); put32(f, 0); fwrite(member, 1, n, f);
    uint32_t cd_size = ftell(f) - cd;
    put32(f, 0x06054b50); put16(f, 0); put16(f, 0); put16(f, 1); put16(f, 1); put32(f, cd_size); put32(f, cd); put16(f, 0);
    assert(!fclose(f));
}

/* Run ipa_chmod in a child (die() exits); return its exit status and its stderr. */
static int run(const char *src, const char *dst, const char *member, char *err, size_t errlen)
{
    int p[2];
    assert(!pipe(p));
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        dup2(p[1], 2);
        exit(ipa_chmod(src, dst, member));
    }
    close(p[1]);
    size_t got = 0;
    ssize_t r;
    while ((r = read(p[0], err + got, errlen - 1 - got)) > 0) got += r;
    err[got] = 0;
    close(p[0]);
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    return WEXITSTATUS(status);
}

/* The external attributes of the first central directory entry. */
static uint32_t external_attr(const char *path)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    unsigned char b[512];
    size_t n = fread(b, 1, sizeof(b), f);
    fclose(f);
    for (size_t i = 0; i + 46 <= n; i++)
        if (rd32(b + i) == 0x02014b50) return rd32(b + i + 38);
    assert(!"no central directory");
    return 0;
}

int main(void)
{
    const char *member = "Payload/Fixture.app/Fixture";
    char err[4096];
    struct stat st;
    write_zip("fixture.ipa", member, "fixture");
    assert(run("fixture.ipa", "prepared.ipa", member, err, sizeof(err)) == 0);
    assert(external_attr("prepared.ipa") >> 16 == 0100755);
    full_disk = true;
    assert(run("fixture.ipa", "prepared.ipa", member, err, sizeof(err)) != 0 && strstr(err, "cannot finish"));
    assert(stat("prepared.ipa", &st) && errno == ENOENT);
    puts("PASS: ipa-chmod, deferred output errors");
}
