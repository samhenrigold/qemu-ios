/* Included by agent-ops.h after agent-sbs.h. The state report: what this daemon tells the host about the guest with
 * every poll, so the host is told when something changes instead of asking, and knows how old what it holds is
 * (README.md, "State report").
 *
 * SpringBoard is asked on a thread of its own. SpringBoardServices' MIG stubs wait without a timeout, and a
 * SpringBoard that is starting, restarting or hung must not hold up the poll that keeps this daemon's lease; that it
 * isn't answering is itself what the host needs to hear then. Every call is picked by its exported name, as launch is
 * (sbs-launch.h), never by firmware. The frameworks themselves are loaded by the main thread (agent_report_load). */
#include <pthread.h>
#include <sys/sysctl.h>

#define AG_REPORT_MAX 1024       /* the host's IT_AGENT_REPORT_MAX: one window */
#define AG_REPORT_PERIOD 250000  /* microseconds between samples, the poll's own period */
#define AG_REPORT_HELD 8         /* polls (2 s) SpringBoard may hold a sample before the report says stalled */

struct ag_report_sample {
    int springboard;             /* the pid of the SpringBoard that answered (1: it answered, its pid unknown); 0: none */
    unsigned char locked, passcode;
    char front[256], name[256];  /* the frontmost app and its localized name; "" on the Home or lock screen */
    int has_uidegrees, uidegrees;   /* SBGetUIOrientation's degrees (3.1.3) */
    int has_interface, interface;   /* SBGetInterfaceOrientation's UIInterfaceOrientation (3.2 on) */
};

static pthread_mutex_t ag_report_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ag_report_sample ag_report_sample;
static unsigned ag_report_asked, ag_report_answered;  /* samples begun and finished: unequal while SpringBoard holds one */

/* One value on its line: no control character can end the line early or start another. */
static void agent_report_value(char *text)
{
    for (; *text; text++) {
        if ((unsigned char)*text < 0x20 || *text == 0x7f) *text = ' ';
    }
}

/* The report for `sample` (0: none finished yet); `stalled` when SpringBoard has held the next one too long, so
 * these lines are the last it answered. At most `size` bytes, never cut inside a line. */
static unsigned agent_report_format(char *out, unsigned size, struct ag_report_sample *sample, int stalled)
{
    char line[600];
    unsigned len = 0;
#define AG_REPORT_LINE(...) do { \
        unsigned n = snprintf(line, sizeof(line), __VA_ARGS__); \
        if (n < sizeof(line) && n <= size - len) { memcpy(out + len, line, n); len += n; } \
    } while (0)
    AG_REPORT_LINE("agent 6\n");
    if (sample) {
        AG_REPORT_LINE("springboard %d\n", sample->springboard);
        if (stalled) AG_REPORT_LINE("stalled 1\n");
    }
    if (sample && sample->springboard) {
        AG_REPORT_LINE("locked %u\npasscode %u\n", sample->locked, sample->passcode);
        if (sample->front[0]) {
            agent_report_value(sample->front);
            AG_REPORT_LINE("front %s\n", sample->front);
            if (sample->name[0]) {
                agent_report_value(sample->name);
                AG_REPORT_LINE("name %s\n", sample->name);
            }
        }
        if (sample->has_uidegrees) AG_REPORT_LINE("uidegrees %d\n", sample->uidegrees);
        if (sample->has_interface) AG_REPORT_LINE("interface %d\n", sample->interface);
    }
#undef AG_REPORT_LINE
    return len;
}

/* The pid of the process whose command name is `name`, 0 when there is none. */
static int agent_report_pid(const char *name)
{
    int mib[] = { CTL_KERN, KERN_PROC, KERN_PROC_ALL }, pid = 0;
    size_t size = 0;
    if (sysctl(mib, 3, 0, &size, 0, 0)) return 0;
    size += 16 * sizeof(struct kinfo_proc);  /* room for what starts between the two calls */
    struct kinfo_proc *procs = malloc(size);
    if (procs && !sysctl(mib, 3, procs, &size, 0, 0)) {
        for (unsigned i = 0; i < size / sizeof(procs[0]) && !pid; i++) {
            if (!strcmp(procs[i].kp_proc.p_comm, name)) pid = procs[i].kp_proc.p_pid;
        }
    }
    free(procs);
    return pid;
}

/* What the sampler calls. Loaded and looked up on the main thread, before the sampler starts: CoreFoundation's
 * initializer traps on any other thread (5.1.1: EXC_BREAKPOINT under dlopen, the agent dead at every start). */
static struct {
    void *sbs;
    void *(*cls)(const char *);
    void *(*sel)(const char *);
    void *(*send)(void *, void *);
    unsigned (*port)(void);
    int (*lock)(unsigned, unsigned char *, unsigned char *);
    void *(*frontmost)(void);
    void *(*name)(void *);
    int (*interface)(unsigned, int *);
    unsigned char (*string)(void *, char *, long, unsigned);
    void (*release)(void *);
} ag_sb;

/* Whether there is a SpringBoardServices to ask here; without one the report stays "agent 6". */
static int agent_report_load(void)
{
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", 2);
    void *foundation = dlopen("/System/Library/Frameworks/Foundation.framework/Foundation", 2);
    void *cf = dlopen("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation", 2);
    void *sbs = dlopen("/System/Library/PrivateFrameworks/SpringBoardServices.framework/SpringBoardServices", 2);
    if (!objc || !foundation || !cf || !sbs) return 0;
    ag_sb.sbs = sbs;
    ag_sb.cls = dlsym(objc, "objc_getClass");
    ag_sb.sel = dlsym(objc, "sel_registerName");
    ag_sb.send = dlsym(objc, "objc_msgSend");
    ag_sb.port = dlsym(sbs, "SBSSpringBoardServerPort");
    ag_sb.lock = dlsym(sbs, "SBGetScreenLockStatus");
    ag_sb.frontmost = dlsym(sbs, "SBSCopyFrontmostApplicationDisplayIdentifier");
    ag_sb.name = dlsym(sbs, "SBSCopyLocalizedApplicationNameForDisplayIdentifier");
    /* 3.2 on, in place of 3.1.3's SBGetUIOrientation (agent_sbs_orientation): what the stock lockdown relay's
     * getInterfaceOrientation calls, with this shape on every build from 7B500 to 11D257. */
    ag_sb.interface = dlsym(sbs, "SBGetInterfaceOrientation");
    ag_sb.string = dlsym(cf, "CFStringGetCString");
    ag_sb.release = dlsym(cf, "CFRelease");
    return ag_sb.cls && ag_sb.sel && ag_sb.send && ag_sb.port && ag_sb.lock && ag_sb.frontmost && ag_sb.string &&
           ag_sb.release;
}

static void *agent_report_sampler(void *unused)
{
    struct ag_report_sample s;
    char named[sizeof(s.front)] = "";  /* the app s.name is of: its name is asked once, when it comes to the front */
    int pid = 0;
    memset(&s, 0, sizeof(s));
    for (;;) {
        struct timeval asked, answered;
        pthread_mutex_lock(&ag_report_lock);
        ag_report_asked++;
        pthread_mutex_unlock(&ag_report_lock);
        void *pool = ag_sb.send(ag_sb.send(ag_sb.cls("NSAutoreleasePool"), ag_sb.sel("alloc")), ag_sb.sel("init"));
        gettimeofday(&asked, 0);
        unsigned sb = ag_sb.port();
        int up = sb && !ag_sb.lock(sb, &s.locked, &s.passcode);
        gettimeofday(&answered, 0);
        /* Which SpringBoard answered is looked up again only when it may be another: none was up, or this answer
         * took its time (where launchd keeps SpringBoard's port, a question asked across a respring waits for the
         * next one and is answered as if nothing happened). */
        long took = (answered.tv_sec - asked.tv_sec) * 1000 + (answered.tv_usec - asked.tv_usec) / 1000;
        if (!up) pid = 0;
        else if (!pid || took < 0 || took >= 500) pid = agent_report_pid("SpringBoard");
        s.springboard = !up ? 0 : pid > 0 ? pid : 1;
        s.front[0] = 0;
        void *identifier = up ? ag_sb.frontmost() : 0;
        if (identifier) {
            if (!ag_sb.string(identifier, s.front, sizeof(s.front), 0x08000100)) s.front[0] = 0;
            if (strcmp(s.front, named)) {
                void *label = ag_sb.name ? ag_sb.name(identifier) : 0;
                if (!label || !ag_sb.string(label, s.name, sizeof(s.name), 0x08000100)) s.name[0] = 0;
                if (label) ag_sb.release(label);
                memcpy(named, s.front, sizeof(named));
            }
            ag_sb.release(identifier);
        }
        s.has_uidegrees = up && !agent_sbs_orientation(ag_sb.sbs, sb, &s.uidegrees);
        s.has_interface = up && ag_sb.interface && !ag_sb.interface(sb, &s.interface) && s.interface >= 1 &&
                          s.interface <= 4;
        if (pool) ag_sb.send(pool, ag_sb.sel("release"));
        pthread_mutex_lock(&ag_report_lock);
        ag_report_sample = s;
        ag_report_answered = ag_report_asked;
        pthread_mutex_unlock(&ag_report_lock);
        usleep(AG_REPORT_PERIOD);
    }
    return 0;
}

/* This poll's report into `out` (AG_REPORT_MAX bytes). The first starts the sampler. */
static unsigned agent_report_now(char *out)
{
    static int started;
    static unsigned held;
    if (!started) {
        pthread_t thread;
        started = 1;
        if (agent_report_load() && !pthread_create(&thread, 0, agent_report_sampler, 0)) pthread_detach(thread);
    }
    pthread_mutex_lock(&ag_report_lock);
    struct ag_report_sample sample = ag_report_sample;
    int sampled = ag_report_answered != 0;
    held = ag_report_asked != ag_report_answered ? held + 1 : 0;
    pthread_mutex_unlock(&ag_report_lock);
    return agent_report_format(out, AG_REPORT_MAX, sampled ? &sample : 0, held >= AG_REPORT_HELD);
}
