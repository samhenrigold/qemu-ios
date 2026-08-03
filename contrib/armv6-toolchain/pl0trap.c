/* Does an unprivileged process actually reach QEMU_CALL?
 *
 * QEMU_CALL is declared PL0_RW in ipod_touch_2g.c and cp_access_ok honours
 * that, so in principle a bare `mcr p15,3,r0,c15,c15,0` from user mode traps to
 * the host with no kernel patch anywhere. That is the whole foundation of a
 * userspace GLES shim, and nobody has ever executed it from PL0 on this
 * machine, so it is worth one measurement rather than one assumption.
 *
 * The test is built so that all three failure modes are distinguishable:
 *
 *   - the mcr faults          -> SIGILL, we never print the result line
 *   - the mcr is a silent nop -> retval stays at the sentinel we wrote
 *   - the host answered       -> retval is QC_GLES_PING_MAGIC
 *
 * The middle case is the one that needs the sentinel: an unhandled cp15 write
 * on this machine does nothing at all rather than faulting, so without a value
 * only the host can produce, "no trap" and "trap that did nothing" look the
 * same.
 */

extern long write(int, const void *, unsigned long);
extern void _exit(int);

#define QC_GLES_PING       0x141
#define QC_GLES_PING_MAGIC 0x6a17c0deLL

/* Mirrors qemu_call_t exactly, same as contrib/it-kbd-agent does:
 * call_number(4) + args(32) + retval(8) + error(8) = 52 bytes packed.
 * The args union is frozen at 32 bytes -- see general.h for why. */
typedef struct __attribute__((packed)) {
    unsigned int  call_number;
    unsigned char args[32];
    long long     retval;
    long long     error;
} qemu_call_t;

static unsigned slen(const char *s) { unsigned n = 0; while (s && s[n]) n++; return n; }
static void w(const char *s) { write(1, s, slen(s)); }

static void wx(unsigned long long v)
{
    char buf[19], *p = buf + 18;
    *p = 0;
    if (!v) *--p = '0';
    while (v) { *--p = "0123456789abcdef"[v & 15]; v >>= 4; }
    w("0x"); w(p);
}

int main(void)
{
    volatile qemu_call_t qc;
    unsigned i;

    for (i = 0; i < sizeof(qc.args); i++) qc.args[i] = 0;
    qc.call_number = QC_GLES_PING;
    qc.retval = 0x0badf00dLL;   /* sentinel: only the host can change this */
    qc.error  = 0;

    w("PL0: about to mcr p15,3,r0,c15,c15,0 from user mode\n");

    /* The register's writefn receives the value we write, and qemu_call() treats
     * it as the guest VA of the request struct. */
    __asm__ __volatile__("mcr p15, 3, %0, c15, c15, 0"
                         : : "r"(&qc) : "memory");

    w("PL0: mcr returned (so it did not fault)\n");
    w("PL0: retval="); wx((unsigned long long)qc.retval); w("\n");

    if (qc.retval == QC_GLES_PING_MAGIC) {
        w("PL0: RESULT=TRAP_WORKS_FROM_USER_MODE\n");
    } else if (qc.retval == 0x0badf00dLL) {
        w("PL0: RESULT=SILENT_NOP (mcr did not reach the host)\n");
    } else {
        w("PL0: RESULT=UNEXPECTED\n");
    }

    _exit(0);
    return 0;
}
