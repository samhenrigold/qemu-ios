/* Restore-only disk imager. It runs in a temporary RAM disk and writes through
 * the guest's ordinary /dev/rdisk0 driver. The optional host transport supplies
 * bytes from one explicitly configured, read-only image stream. */
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/disk.h>
#include "../armv6-toolchain/qemu-call.h"
#ifdef IT_INITIAL_PREPARE
#include "data.h"
#endif
extern int reboot(int);
static unsigned char buffer[65536];
static unsigned long long source_offset;
static int get(void *out, unsigned n)
{
    qemu_call_t q;
    unsigned done = 0;
    /* Fault destination pages in at the guest's privilege before host copy. */
    memset(out, 0, n);
    while (done < n) {
        memset(&q, 0, sizeof(q));
        q.call_number = 0x500;
        q.ag.buffer = (unsigned int)((char *)out + done);
        q.ag.length = n - done;
        q.ag.token = source_offset;
        qemu_call_trap(&q);
        if (q.retval <= 0 || q.retval > n - done || q.error)
            return -1;
        source_offset += q.retval;
        done += q.retval;
    }
    return 0;
}
static unsigned crc_table[256];
static unsigned crc(const unsigned char *p, unsigned n)
{
    unsigned c = ~0U, i;
    for (i = 0; i < n; i++)
        c = (c >> 8) ^ crc_table[(c ^ p[i]) & 255];
    return ~c;
}
static void die(const char *what)
{
    printf("A6RESTORE FAIL %s errno=%d source=%llu\n", what, errno,
           source_offset);
    fflush(stdout);
    for (;;)
        sleep(60);
}
int main(void)
{
    struct {
        char magic[4];
        unsigned version;
        unsigned long long size;
        unsigned records, reserved;
    } h;
    struct {
        unsigned long long target;
        unsigned length, checksum;
    } r;
    unsigned i;
    unsigned long long total = 0;
    setbuf(stdout, 0);
    setbuf(stderr, 0);
    printf("A6RESTORE START\n");
    for (i = 0; i < 256; i++) {
        unsigned c = i, k;
        for (k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xedb88320U & (0U - (c & 1)));
        crc_table[i] = c;
    }
    if (get(&h, sizeof h) || memcmp(h.magic, "A6DI", 4) || h.version != 1 ||
        h.records > 1000000 || (h.reserved & ~1U))
        die("stream header");
    unsigned verify_only = h.reserved & 1;
    int fd = -1;
    for (i = 0; i < 60 && fd < 0; i++) {
        fd = open("/dev/rdisk0", O_RDWR);
        if (fd < 0)
            sleep(1);
    }
    if (fd < 0)
        die("open /dev/rdisk0");
    uint64_t blocks = 0;
    uint32_t block_size = 0;
    if (ioctl(fd, DKIOCGETBLOCKCOUNT, &blocks) ||
        ioctl(fd, DKIOCGETBLOCKSIZE, &block_size) || block_size != 4096 ||
        blocks * block_size != h.size)
        die("target media geometry");
    printf("A6RESTORE MEDIA blocks=%llu block_size=%u\n", blocks, block_size);
    if (pread(fd, buffer, 4096, 0) != 4096)
        die("initial disk read");
    printf("A6RESTORE BEFORE magic=%02x%02x%02x%02x mbr=%02x%02x crc=%08x\n",
           buffer[0], buffer[1], buffer[2], buffer[3], buffer[510], buffer[511],
           crc(buffer, 4096));
    for (i = 0; i < h.records; i++) {
        if (get(&r, sizeof r) || !r.length || r.length > sizeof buffer ||
            (r.target & 4095) || (r.length & 4095) || r.target > h.size ||
            r.length > h.size - r.target)
            die("record header");
        if (get(buffer, r.length) || crc(buffer, r.length) != r.checksum)
            die("record payload");
        unsigned done = 0;
        while (done < r.length) {
            ssize_t n = verify_only ? pread(fd, buffer + done, r.length - done,
                                            (off_t)(r.target + done))
                                    : pwrite(fd, buffer + done, r.length - done,
                                             (off_t)(r.target + done));
            if (n <= 0)
                die(verify_only ? "disk verify read" : "disk write");
            done += n;
        }
        if (verify_only && crc(buffer, r.length) != r.checksum)
            die("disk verify checksum");
        total += r.length;
        if (!(i % 512))
            printf("A6RESTORE PROGRESS %u/%u bytes=%llu target=%llu\n", i + 1,
                   h.records, total, r.target);
    }
    if (fsync(fd))
        die("disk fsync");
    if (ioctl(fd, DKIOCSYNCHRONIZECACHE))
        die("disk cache flush");
    if (h.records) {
        if (pread(fd, buffer, r.length, (off_t)r.target) != r.length ||
            crc(buffer, r.length) != r.checksum)
            die("disk readback");
        printf("A6RESTORE READBACK OK target=%llu crc=%08x\n", r.target,
               r.checksum);
    }
    close(fd);
    sync();
    printf("A6RESTORE DONE records=%u bytes=%llu verify_only=%u\n", h.records,
           total, verify_only);
#ifdef IT_INITIAL_PREPARE
    /* The first normal boot must see protected files created after its keybag. */
    (void)it_prepare_data();
    die("native Data preparation returned");
#else
    printf(
        "A6RESTORE HALTING to checkpoint the native flash translation layer\n");
    reboot(
        0x408); /* RB_HALT | RB_QUICK: sync caches and close drivers, avoid launchd termination race. */
    die("halt returned");
#endif
    return 1;
}
