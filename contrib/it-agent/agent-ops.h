/* Included by it_agent.c after the clipboard bridge and cp15 ABI. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <spawn.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/stat.h>


#define AG_REQUEST_MAX (256 * 1024)
#define AG_RESPONSE_MAX (1024 * 1024)
static char ag_request[AG_REQUEST_MAX + 1];
static unsigned char ag_response[AG_RESPONSE_MAX];
static unsigned ag_response_len;
static pid_t ag_child;
static int ag_output = -1;
static unsigned ag_ticks;
static int ag_child_status;
static int ag_reaped;

static void agent_cancel(void)
{
    if (ag_child) {
        kill(-ag_child, SIGKILL);
        if (!ag_reaped) waitpid(ag_child, 0, 0);
    }
    if (ag_output >= 0) close(ag_output);
    ag_child = 0;
    ag_output = -1;
    ag_response_len = 0;
}

static void agent_done(int status)
{
    unsigned off = 0;
    do {
        unsigned n = ag_response_len - off;
        if (n > 1024) n = 1024;
        if (qc(0x163, ag_response + off, off, n) != n) {
            agent_token = 0;
            return;
        }
        off += n;
    } while (off < ag_response_len);
    if (qc(0x164, 0, (uint32_t)status, 0) < 0) agent_token = 0;
    ag_response_len = 0;
}

static int write_all(int fd, const void *bytes, unsigned len)
{
    const char *p = bytes;
    while (len) {
        long n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; len -= n;
    }
    return 0;
}

static int agent_exec(char *command, const char *body, unsigned len)
{
    char tmp[] = "/tmp/it-agent-stdin.XXXXXX";
    int input = mkstemp(tmp), fds[2], error;
    posix_spawn_file_actions_t actions;
    posix_spawnattr_t attr;
    if (input < 0) return errno;
    unlink(tmp);
    if (write_all(input, body, len) || lseek(input, 0, SEEK_SET) < 0 || pipe(fds)) {
        error = errno; close(input); return error;
    }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input, 0);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 1);
    posix_spawn_file_actions_adddup2(&actions, fds[1], 2);
    posix_spawn_file_actions_addclose(&actions, input);
    posix_spawn_file_actions_addclose(&actions, fds[0]);
    posix_spawn_file_actions_addclose(&actions, fds[1]);
    posix_spawnattr_init(&attr);
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
    char *argv[] = { "/bin/sh", "-c", command, 0 };
    char *environment[] = { "PATH=/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin",
                            "HOME=/var/root", "LANG=C", "LC_ALL=C", 0 };
    error = posix_spawn(&ag_child, "/bin/sh", &actions, &attr, argv, environment);
    posix_spawn_file_actions_destroy(&actions);
    posix_spawnattr_destroy(&attr);
    close(input); close(fds[1]);
    if (error) { close(fds[0]); ag_child = 0; return error; }
    ag_output = fds[0];
    ag_ticks = 0; ag_reaped = 0; ag_child_status = 0;
    return 0;
}

static void agent_child_tick(void)
{
    unsigned char buffer[4096];
    int eof = 0, error = 0;
    /* Bound work per tick so clipboard and the lease keep moving. */
    for (unsigned i = 0; i < 16; i++) {
        long n = read(ag_output, buffer, sizeof(buffer));
        if (!n) { eof = 1; break; }
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno != EAGAIN) error = errno;
            break;
        }
        if (n > AG_RESPONSE_MAX - ag_response_len) { error = EFBIG; break; }
        memcpy(ag_response + ag_response_len, buffer, n);
        ag_response_len += n;
    }
    if (!ag_reaped) {
        pid_t pid = waitpid(ag_child, &ag_child_status, WNOHANG);
        if (pid == ag_child) ag_reaped = 1;
        else if (pid < 0 && errno != EINTR) error = errno;
    }
    if (++ag_ticks >= 240) error = ETIMEDOUT;
    if (error || (eof && ag_reaped)) {
        int status = error ? -error : WIFEXITED(ag_child_status) ?
            WEXITSTATUS(ag_child_status) : 128 + WTERMSIG(ag_child_status);
        /* Also reap descendants that kept the output pipe open. */
        kill(-ag_child, SIGKILL);
        if (!ag_reaped) waitpid(ag_child, 0, 0);
        close(ag_output); ag_output = -1; ag_child = 0;
        agent_done(status);
    }
}

#include "agent-sbs.h"

static void agent_dispatch(unsigned size)
{
    char *nl = memchr(ag_request, '\n', size);
    if (!nl) { agent_done(-EINVAL); return; }
    *nl = 0;
    char *op = strchr(ag_request, ' ');
    if (!op) { agent_done(-EINVAL); return; }
    op++;
    char *args = strchr(op, ' ');
    if (args) *args++ = 0;
    else args = op + strlen(op);
    const char *body = nl + 1;
    unsigned body_len = size - (body - ag_request);
    int status = 0;
    if (!strcmp(op, "ping")) {
        memcpy(ag_response, "it_agent v1\n", 12); ag_response_len = 12;
    } else if (!strcmp(op, "exec")) {
        status = agent_exec(args, body, body_len);
        if (!status) return;
        status = -status;
    } else if (!strcmp(op, "type") || !strcmp(op, "backspace") || !strcmp(op, "uidump")) {
        if (body_len > 65536) status = -EFBIG;
        else status = agent_sbs(op, args);
        if (!status) return; /* The target process commits the result. */
    } else if (!strcmp(op, "launch") || !strcmp(op, "frontmost") || !strcmp(op, "lockstatus") || !strcmp(op, "orientation")) {
        status = agent_sbs(op, args);
    } else if (!strcmp(op, "halt")) {
        extern int reboot2(int, const char *);
        if (reboot2(8, 0)) status = -errno;
    } else if (!strcmp(op, "kill")) {
        /* Quote a single executable name; never interpolate it as shell code. */
        char command[4096];
        unsigned n = 0;
        const char *prefix = "killall '";
        if (!*args || *args == '-' || strlen(args) > 512) status = -EINVAL;
        else {
            memcpy(command, prefix, strlen(prefix)); n = strlen(prefix);
            for (const char *p = args; *p; p++) {
                if (*p == 39) { memcpy(command + n, "'\\''", 4); n += 4; }
                else command[n++] = *p;
            }
            command[n++] = 39; command[n] = 0;
            status = agent_exec(command, body, body_len);
            if (!status) return;
            status = -status;
        }
    } else if (!strcmp(op, "settime")) {
        char *end;
        errno = 0;
        long epoch = strtol(args, &end, 10);
        struct timeval tv = { epoch, 0 };
        if (errno || !*args || *end || epoch <= 0) status = -EINVAL;
        else if (settimeofday(&tv, 0)) status = -errno;
    } else if (!strcmp(op, "get") || !strcmp(op, "getrange")) {
        long long offset = 0;
        unsigned limit = AG_RESPONSE_MAX;
        int ranged = !strcmp(op, "getrange");
        if (ranged) {
            char *end;
            errno = 0;
            offset = strtoll(args, &end, 10);
            if (errno || end == args || *end != ' ' || offset < 0) {
                agent_done(-EINVAL); return;
            }
            args = end + 1;
            errno = 0;
            unsigned long requested = strtoul(args, &end, 10);
            if (errno || end == args || *end != ' ' || !end[1] ||
                requested > AG_RESPONSE_MAX) {
                agent_done(-EINVAL); return;
            }
            limit = requested;
            args = end + 1;
        }
        int fd = open(args, O_RDONLY | O_NOFOLLOW);
        struct stat st;
        if (fd < 0) status = -errno;
        else {
            if (fstat(fd, &st)) status = -errno;
            else if (!S_ISREG(st.st_mode) || st.st_size < 0) status = -EINVAL;
            else if (!ranged && st.st_size > AG_RESPONSE_MAX) status = -EFBIG;
            else if (offset < st.st_size) {
                if (st.st_size - offset < limit) limit = st.st_size - offset;
                if (lseek(fd, offset, SEEK_SET) < 0) status = -errno;
                while (!status && ag_response_len < limit) {
                    long n = read(fd, ag_response + ag_response_len, limit - ag_response_len);
                    if (n < 0 && errno == EINTR) continue;
                    if (n <= 0) { status = n < 0 ? -errno : -EIO; break; }
                    ag_response_len += n;
                }
            }
            close(fd);
        }
    } else if (!strcmp(op, "put")) {
        char *mode = strrchr(args, ' '), *end;
        if (!mode) status = -EINVAL;
        else {
            *mode++ = 0;
            long permissions = strtol(mode, &end, 8);
            char tmp[4096];
            if (!*args || !*mode || *end || permissions < 0 || permissions > 0777 ||
                snprintf(tmp, sizeof(tmp), "%s.it-agent.XXXXXX", args) >= sizeof(tmp)) status = -EINVAL;
            else {
                int fd = mkstemp(tmp);
                if (fd < 0) status = -errno;
                else {
                    if (write_all(fd, body, body_len) || fchmod(fd, permissions) || fsync(fd)) status = -errno;
                    if (close(fd) && !status) status = -errno;
                    if (!status && rename(tmp, args)) status = -errno;
                    if (status) unlink(tmp);
                }
            }
        }
    } else status = -ENOSYS;
    agent_done(status);
}

static void agent_tick(void)
{
    static unsigned tick;
    if (!agent_token) {
        agent_cancel();
        int64_t token = qc(0x160, 0, 0, 0);
        if (token <= 0) return;
        agent_token = token;
    }
    int64_t size = qc(0x161, 0, 0, 0);
    if (size < 0) { agent_token = 0; agent_cancel(); return; }
    if (!(++tick % 4)) {
        int64_t epoch = qc(0x165, 0, 0, 0);
        struct timeval tv;
        gettimeofday(&tv, 0);
        if (epoch > 0 && llabs(epoch - tv.tv_sec) > 2) {
            tv.tv_sec = epoch; tv.tv_usec = 0; settimeofday(&tv, 0);
        }
    }
    if (ag_child) { agent_child_tick(); return; }
    if (!size) return;
    if (size > AG_REQUEST_MAX) { agent_done(-EFBIG); return; }
    /* cpu_memory_rw_debug cannot fault in demand-zero guest pages. */
    memset(ag_request, 0, size + 1);
    for (unsigned off = 0; off < size;) {
        unsigned len = size - off;
        if (len > 1024) len = 1024;
        if (qc(0x162, ag_request + off, off, len) != len) {
            agent_done(-EIO); return;
        }
        off += len;
    }
    ag_request[size] = 0;
    agent_dispatch(size);
}
