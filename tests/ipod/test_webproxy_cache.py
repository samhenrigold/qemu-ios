#!/usr/bin/env python3
"""Offline cache publication and failed-write cleanup; no sockets or guest."""
from pathlib import Path
import os
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / 'contrib/it-webproxy/itwebproxy.c'
KEY = 'https://archive.example.invalid/web/20090909id_/http://fixture.invalid/'
RESPONSE = (b'HTTP/1.0 200 Archive response\r\nContent-Type: text/plain\r\n'
            b'Content-Length: 19\r\nConnection: close\r\n\r\nnew cached response')


with tempfile.TemporaryDirectory(prefix='it-webproxy-cache-') as directory:
    work = Path(directory)
    check = work / 'cache.c'
    check.write_text(r'''
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static ssize_t test_write(int, const void *, size_t);
static int test_close(int);
static int test_rename(const char *, const char *);
#define write test_write
#define close test_close
#define rename test_rename
#define main proxy_main
''' + '#include "' + str(SOURCE) + '"\n' + r'''
#undef main
#undef rename
#undef close
#undef write

static int failure;
static size_t written;
static char slot[PATH_MAX];
static char previous[4096];
static ssize_t previous_size;

static void check_previous(void)
{
    char data[sizeof(previous)];
    int fd = open(slot, O_RDONLY);
    if (previous_size < 0) {
        assert(fd < 0 && errno == ENOENT);
    } else {
        assert(fd >= 0);
        assert(read(fd, data, sizeof(data)) == previous_size);
        assert(!memcmp(data, previous, (size_t)previous_size));
        assert(close(fd) == 0);
    }
}
static ssize_t test_write(int fd, const void *data, size_t size)
{
    if (fd == STDOUT_FILENO) return write(fd, data, size);
    check_previous();
    if (failure == 1) {
        /* Complete the key and some headers before simulating a full disk. */
        if (written >= 128) { errno = ENOSPC; return -1; }
        if (size > 128 - written) size = 128 - written;
    }
    ssize_t count = write(fd, data, size);
    if (count > 0) written += (size_t)count;
    return count;
}
static int test_close(int fd)
{
    bool writing = (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDONLY;
    if (writing) check_previous();
    int result = close(fd);
    if (writing && failure == 2) { errno = ENOSPC; return -1; }
    return result;
}
static int test_rename(const char *from, const char *to)
{
    check_previous();
    if (failure == 3) { errno = EIO; return -1; }
    return rename(from, to);
}
int main(int argc, char **argv)
{
    assert(argc == 5);
    assert(archive_cache_path(argv[2], argv[3], slot, sizeof(slot)));
    if (!strcmp(argv[1], "path")) { puts(slot); return 0; }
    if (!strcmp(argv[1], "read")) {
        int fd = archive_cache(argv[2], argv[3]);
        bool found = archive_cached(fd, argv[3]);
        if (fd >= 0) close(fd);
        return found ? 0 : 2;
    }
    int fd = open(slot, O_RDONLY);
    previous_size = fd < 0 ? -1 : read(fd, previous, sizeof(previous));
    if (fd >= 0) close(fd);
    failure = atoi(argv[4]);
    ArchiveReply reply = {0};
    strcpy(reply.headers, "Content-Type: text/plain\r\n");
    reply.headers_len = strlen(reply.headers);
    reply.body = "new cached response";
    reply.body_len = strlen(reply.body);
    if (failure == 4) reply.body_len = CACHE_MAX;
    bool saved = archive_store(argv[2], argv[3], &reply, 200);
    puts(saved ? "saved" : "skipped");
    return 0;
}
''')
    helper = work / 'cache'
    subprocess.run(['cc', '-g', '-Wall', '-Wextra', '-Werror',
                    '-Wno-deprecated-declarations', '-fsanitize=address,undefined',
                    str(check), '-lcurl', '-o', str(helper)], check=True)

    def run(operation, config, failure=0, key=KEY):
        result = subprocess.run([str(helper), operation, str(config), key, str(failure)],
                                capture_output=True, timeout=10)
        assert not result.stderr, result.stderr
        return result

    for failure in range(5):
        case = work / f'case-{failure}'
        case.mkdir()
        config = case / 'configuration with spaces'
        slot = Path(run('path', config).stdout.decode().strip())
        # A cache miss must not create an empty slot or staging file.
        assert run('read', config).returncode == 2
        assert list(case.iterdir()) == []
        old = b'previous committed slot'
        slot.write_bytes(old)
        unrelated = case / 'unrelated.XXXXXX'
        unrelated.write_bytes(b'keep')
        result = run('store', config, failure)
        assert result.returncode == 0, result
        assert set(case.iterdir()) == {slot, unrelated}
        assert unrelated.read_bytes() == b'keep'
        if failure:
            assert result.stdout == b'skipped\n', result
            assert slot.read_bytes() == old
        else:
            assert result.stdout == b'saved\n', result
            assert slot.read_bytes() == KEY.encode() + b'\n' + RESPONSE
            assert slot.stat().st_mode & 0o777 == 0o600
            cached = run('read', config)
            assert cached.returncode == 0 and cached.stdout == RESPONSE, cached
            os.utime(slot, (1, 1))
            assert run('read', config).returncode == 2

    first = work / 'first-entry'
    first.mkdir()
    config = first / 'configuration'
    result = run('store', config)
    assert result.returncode == 0 and result.stdout == b'saved\n', result
    slot = Path(run('path', config).stdout.decode().strip())
    assert list(first.iterdir()) == [slot]
    assert slot.read_bytes() == KEY.encode() + b'\n' + RESPONSE

    # Failure before a staging file exists must remain an optional cache miss.
    result = run('store', work / 'missing-directory/configuration')
    assert result.returncode == 0 and result.stdout == b'skipped\n', result
    assert not (work / 'missing-directory').exists()

print('PASS: atomic cache publication, write/close/rename failure cleanup, bounded entries and cache misses')
