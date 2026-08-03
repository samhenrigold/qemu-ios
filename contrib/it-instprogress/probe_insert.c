/* probe_insert: does DYLD_INSERT_LIBRARIES still work on 3.1.3, from a launchd
 * plist, into itunesstored?
 *
 * That is the one load-bearing unknown for the progress bar. The bar is fed by
 * an ISDownload that only the owner of the Mach service
 * com.apple.iTunesStore.daemon-notifications may publish, launchd hands that
 * receive right to itunesstored and nobody else, and so the only way to post
 * one is to be running inside itunesstored. Everything else about that plan is
 * already known to be constructible -- ISOperationProgress, ISDownloadStatus
 * and ISDownload are all exported ObjC classes with initWithCoder:/
 * encodeWithCoder: -- so this is what decides whether the plan is real.
 *
 * It answers only that question. A constructor writes a line to
 * /tmp/it-insert-probe and gets out of the way; it does not touch the daemon's
 * behaviour, which is deliberate, because a probe that changes what it
 * measures is worth nothing.
 */
extern long write(int, const void *, unsigned long);
extern int open(const char *, int, ...);
extern int close(int);
extern int getpid(void);

#define O_WRONLY 0x0001
#define O_CREAT 0x0200
#define O_APPEND 0x0008

static unsigned utoa(unsigned v, char *out)
{
    char tmp[12];
    unsigned n = 0, i = 0;
    if (!v) tmp[n++] = '0';
    while (v) { tmp[n++] = '0' + (v % 10); v /= 10; }
    while (n) out[i++] = tmp[--n];
    return i;
}

__attribute__((constructor)) static void probe_loaded(void)
{
    char line[64];
    unsigned n = 0;
    const char *tag = "loaded in pid ";

    while (tag[n]) { line[n] = tag[n]; n++; }
    n += utoa((unsigned)getpid(), line + n);
    line[n++] = '\n';

    int fd = open("/tmp/it-insert-probe", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (fd >= 0) { write(fd, line, n); close(fd); }
}
