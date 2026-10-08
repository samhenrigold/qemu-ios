/* Exercise the production bounded trace without extra guest transactions.
 *
 * The trace keeps its selector and record count in statics, so each configuration runs in a fresh child process
 * whose guest-visible summary and trace go to their own files.
 *
 * SLICE include/hw/arm/ipod_touch_fmss.h define FMSS
 * SLICE hw/arm/ipod_touch_fmss.c define FMSS_CHIP|FMSS_SCRIPT
 * SLICE hw/arm/ipod_touch_fmss.c fn fmss_script_trace fmss_var_read fmss_run_script
 */
#define FMSS_SCRIPT_TRACE_TEST
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/wait.h>
#include <unistd.h>
static FILE *trace_err;
#define TRACE_PRINTF(fn, ...) fprintf(trace_err, __VA_ARGS__)
static bool enabled;
static bool fmss_script_trace_on(void) { return enabled; }
#include "ipod-fmss-script.h"
#include "slice.h"

static void scenario(bool write, FILE *out) {
    const uint32_t program[] = {
        0x05000000, 0xa000, 0x03010000, 0,
        0x02010804, 0, 0x01000008, 0x90,
        0x01000000, 2, 0x01000030, 7,
        0x01000014, 0x10, 0x01000004, 0xe2,
        0x01000040, 0x82, 0x04000060, 0xffffffff,
        0x05070000, 0x9000, 0x11000007, 0, 0, 0
    };
    IPodTouchFMSSState s = {.reg_cs_script = 0x1000,
                            .reg_csgenrc = write ? 0xa02 : 0xa01};
    stl_le_p(mem + 0xa000, 0xabcddcba);
    memcpy(mem + 0x1000, program, sizeof(program));
    fmss_run_script(&s);
    assert(ldl_le_p(mem + 0x9000) == FMSS_CHIP_ID);
    fprintf(out, "reads=%u writes=%u descriptor=%u id=%08x\n", reads, writes,
            descriptor_reads, ldl_le_p(mem + 0x9000));
    /* Stress the exact production cap; no extra model/guest access. */
    for (unsigned i = 0; i < FMSS_SCRIPT_TRACE_LIMIT + 9; i++) {
        fmss_script_trace("probe", 0x1000, i, 0, 0, s.reg_csgenrc);
    }
}

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *text = calloc(n + 1, 1);
    assert(fread(text, 1, n, f) == (size_t)n);
    fclose(f);
    return text;
}

typedef struct { char *out, *err; } Run;

/* selector NULL: FMSS_SCRIPT_TRACE_CSGENRC unset. */
static Run run(const char *selector, bool on, bool write) {
    static int n;
    char out[32], err[32];
    snprintf(out, sizeof(out), "out.%d", n);
    snprintf(err, sizeof(err), "err.%d", n++);
    fflush(NULL);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        if (selector) setenv("FMSS_SCRIPT_TRACE_CSGENRC", selector, 1);
        else unsetenv("FMSS_SCRIPT_TRACE_CSGENRC");
        enabled = on;
        FILE *o = fopen(out, "w");
        trace_err = fopen(err, "w");
        assert(o && trace_err);
        scenario(write, o);
        fclose(o);
        fclose(trace_err);
        _exit(0);
    }
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && !WEXITSTATUS(status));
    return (Run){slurp(out), slurp(err)};
}

int main(void) {
    Run disabled = run(NULL, false, false), on = run(NULL, true, false);
    assert(!*disabled.err);
    assert(!strcmp(on.out, disabled.out));
    unsigned lines = 0, truncated = 0;
    bool unmodeled = false, descriptor = false, store = false;
    const char *last = NULL;
    for (char *line = on.err, *end; *line; line = end + 1) {
        end = strchr(line, '\n');
        assert(end);
        *end = 0;
        lines++;
        last = line;
        truncated += strstr(line, "truncated") != NULL;
        unmodeled |= strstr(line, "fmc_write_unmodeled") && strstr(line, "arg=00000804");
        descriptor |= strstr(line, "descriptor_read") && strstr(line, "arg=0000a000 value=00000004");
        store |= strstr(line, "store") && strstr(line, "arg=00009000 value=00000004");
        assert(!strstr(line, "abcddcba") && !strstr(line, "b614d5ad"));
        *end = '\n';
    }
    assert(lines == 16385);
    assert(!strcmp(last, "FMSS_SCRIPT_TRACE truncated limit=16384\n"));
    assert(truncated == 1 && unmodeled && descriptor && store);
    Run ignored = run("0xa02", true, false);
    assert(!strcmp(ignored.out, disabled.out) && !*ignored.err);
    Run selected = run("0xa02", true, true);
    assert(!strcmp(selected.out, disabled.out) && !strcmp(selected.err, on.err));
    const char *invalid[] = {"", "-1", "0xa02junk", "0x100000000", "nonsense"};
    for (unsigned i = 0; i < sizeof(invalid) / sizeof(invalid[0]); i++) {
        Run refused = run(invalid[i], true, false);
        assert(!strcmp(refused.out, disabled.out));
        assert(!strcmp(refused.err, "FMSS_SCRIPT_TRACE invalid CSGENRC selector; tracing disabled\n"));
    }
    puts("PASS selector nonmatch silence, match parity, invalid single refusal; trace disabled silence, identical guest reads/writes/results, unsupported auxiliary access, payload exclusion, fixed cap and single overflow marker");
}
