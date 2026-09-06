/* One slirp guestfwd connection on stdin/stdout. No host listening socket.
 * Direct HTTP uses the host resolver; CONNECT remains end-to-end guest TLS.
 * An upstream HTTP proxy (including WaybackProxy) gets an unmodified stream.
 */
#include <curl/curl.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/file.h>
#include <time.h>
#include <limits.h>
#include <stdint.h>
#include <sys/stat.h>
#include <stdarg.h>
#include <unistd.h>
#ifdef HAVE_OPENSSL
#include "tls-bridge.h"
#endif
#include <unistd.h>

#define HEAD_MAX 65536
#define BODY_MAX (8 * 1024 * 1024)
static void client_finish(void);
static bool sendall(int fd, const void *data, size_t n);
static int reply_printf(int fd, const char *format, ...);
static void fail(int code, const char *message)
{
    reply_printf(1, "HTTP/1.0 %d %s\r\nConnection: close\r\nContent-Type: text/plain\r\n\r\n%s\n", code, message, message);
    client_finish(); exit(1);
}
/* Exact retired API endpoints only; never block a vendor's whole domain.
 * See README for retirement sources. Archive/off/upstream modes bypass this. */
static bool retired_host(const char *host)
{
    static const char *const retired[] = { "api.openfeint.com", "gdata.youtube.com" };
    size_t length = strlen(host);
    if (length && host[length - 1] == '.') length--;
    for (size_t i = 0; i < sizeof(retired) / sizeof(retired[0]); i++) {
        if (length == strlen(retired[i]) && !strncasecmp(host, retired[i], length)) return true;
    }
    return false;
}

static bool retired_url(const char *target)
{
    CURLU *url = curl_url();
    char *host = NULL;
    bool retired = url && !curl_url_set(url, CURLUPART_URL, target, 0) &&
        !curl_url_get(url, CURLUPART_HOST, &host, 0) && retired_host(host);
    curl_free(host);
    curl_url_cleanup(url);
    return retired;
}

static bool sendall(int fd, const void *data, size_t n)
{
    const char *p = data;
    while (n) {
        struct pollfd f = {fd, POLLOUT, 0};
        if (poll(&f, 1, 30000) <= 0) return false;
        ssize_t k;
#ifdef HAVE_OPENSSL
        if (fd == 1 && client_tls) {
            k = SSL_write(client_tls, p, n > INT_MAX ? INT_MAX : (int)n);
            if (k <= 0) return false;
        }
        else
#endif
        k = write(fd, p, n);
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k <= 0) return false;
        p += k; n -= k;
    }
    return true;
}
static int reply_printf(int fd, const char *format, ...)
{
    va_list args;
    va_start(args, format);
    char *text = NULL;
    int count = vasprintf(&text, format, args);
    va_end(args);
    bool ok = count >= 0 && sendall(fd, text, (size_t)count);
    free(text);
    return ok ? count : -1;
}
static ssize_t client_read(void *data, size_t count)
{
#ifdef HAVE_OPENSSL
    if (client_tls) {
        int n = SSL_read(client_tls, data, count > INT_MAX ? INT_MAX : (int)count);
        if (n < 0) errno = EIO;
        return n;
    }
#endif
    return read(0, data, count);
}
static void client_finish(void)
{
#ifdef HAVE_OPENSSL
    if (client_tls) { SSL_shutdown(client_tls); SSL_free(client_tls); client_tls = NULL; }
#endif
}
static int connect_host(const char *host, const char *port)
{
    struct addrinfo hints = {.ai_socktype = SOCK_STREAM}, *list = NULL;
    if (getaddrinfo(host, port, &hints, &list)) return -1;
    int fd = -1;
    for (struct addrinfo *a = list; a; a = a->ai_next) {
        fd = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (fd < 0) continue;
        fcntl(fd, F_SETFL, O_NONBLOCK);
        int rc = connect(fd, a->ai_addr, a->ai_addrlen);
        struct pollfd p = {fd, POLLOUT, 0};
        int err = 0; socklen_t size = sizeof(err);
        if (rc == 0 || (errno == EINPROGRESS && poll(&p, 1, 10000) > 0 &&
                       getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &size) == 0 && !err)) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(list);
    return fd;
}
static void relay(int socket)
{
    bool input = true, remote = true;
    char buf[16384];
    while (remote) {
        struct pollfd p[2] = {{input ? 0 : -1, POLLIN, 0}, {socket, POLLIN, 0}};
        if (poll(p, 2, 60000) <= 0) break;
        for (int i = 0; i < 2; i++) {
            if (!(p[i].revents & (POLLIN | POLLHUP | POLLERR))) continue;
            ssize_t n = read(p[i].fd, buf, sizeof(buf));
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            if (n <= 0) {
                if (i == 0) { input = false; shutdown(socket, SHUT_WR); }
                else remote = false;
            } else if (!sendall(i == 0 ? socket : 1, buf, n)) return;
        }
    }
}
static bool header_is(const char *s, const char *name)
{
    return !strncasecmp(s, name, strlen(name)) && s[strlen(name)] == ':';
}
static bool hop(const char *s)
{
    return header_is(s, "Connection") || header_is(s, "Proxy-Connection") ||
        header_is(s, "Keep-Alive") || header_is(s, "Transfer-Encoding") ||
        header_is(s, "TE") || header_is(s, "Trailer") || header_is(s, "Upgrade") ||
        header_is(s, "Proxy-Authorization") || header_is(s, "Proxy-Authenticate");
}
static size_t output(void *data, size_t size, size_t count, void *ctx)
{
    (void)ctx;
    size_t n = size * count;
    return sendall(1, data, n) ? n : 0;
}
static size_t response_header(char *data, size_t size, size_t count, void *ctx)
{
    bool *started = ctx;
    size_t n = size * count;
    if (n >= HEAD_MAX) return 0;
    *started = true;
    char line[HEAD_MAX]; memcpy(line, data, n); line[n] = 0;
    if (hop(line) || header_is(line, "Content-Length")) return n;
    if (!strcmp(line, "\r\n")) {
        const char end[] = "Connection: close\r\n\r\n";
        return sendall(1, end, sizeof(end) - 1) ? n : 0;
    }
    return sendall(1, data, n) ? n : 0;
}
#ifndef ARCHIVE_ORIGIN
#define ARCHIVE_ORIGIN "https://web.archive.org"
#endif
#define ARCHIVE_BODY_MAX (32 * 1024 * 1024)
typedef struct ArchiveReply {
    char headers[HEAD_MAX], location[16384];
    size_t headers_len, body_len, body_capacity;
    long retry_after;
    bool textual;
    char *body;
} ArchiveReply;
static size_t archive_body(char *data, size_t size, size_t count, void *opaque)
{
    ArchiveReply *reply = opaque;
    size_t n = size * count;
    if (n > ARCHIVE_BODY_MAX - reply->body_len) return 0;
    size_t needed = reply->body_len + n;
    if (needed > reply->body_capacity) {
        size_t capacity = reply->body_capacity ? reply->body_capacity : 16384;
        while (capacity < needed) capacity = capacity > ARCHIVE_BODY_MAX / 2 ? ARCHIVE_BODY_MAX : capacity * 2;
        char *body = realloc(reply->body, capacity);
        if (!body) return 0;
        reply->body = body; reply->body_capacity = capacity;
    }
    if (n) memcpy(reply->body + reply->body_len, data, n);
    reply->body_len += n;
    return n;
}
static size_t archive_header(char *data, size_t size, size_t count, void *opaque)
{
    ArchiveReply *reply = opaque;
    size_t n = size * count;
    if (n >= HEAD_MAX) return 0;
    char line[HEAD_MAX]; memcpy(line, data, n); line[n] = 0;
    if (!strncasecmp(line, "HTTP/", 5)) {
        reply->headers_len = 0; reply->body_len = 0; reply->location[0] = 0;
        return n;
    }
    if (header_is(line, "Retry-After")) {
        const char *value = strchr(line, ':') + 1;
        while (*value == ' ' || *value == '\t') value++;
        char *end;
        long seconds = strtol(value, &end, 10);
        if (end == value || (*end != '\r' && *end != '\n' && *end)) {
            time_t date = curl_getdate(value, NULL);
            seconds = date == (time_t)-1 ? 60 : (long)(date - time(NULL));
        }
        reply->retry_after = seconds > 0 && seconds < 31536000 ? seconds : 60;
    }
    if (header_is(line, "Content-Type")) {
        const char *value = strchr(line, ':') + 1;
        while (*value == ' ' || *value == '\t') value++;
        reply->textual = !strncasecmp(value, "text/html", 9) ||
            !strncasecmp(value, "text/css", 8) || !strncasecmp(value, "text/javascript", 15) ||
            !strncasecmp(value, "application/javascript", 22) ||
            !strncasecmp(value, "application/xhtml+xml", 21);
    }
    if (header_is(line, "Location")) {
        const char *value = strchr(line, ':') + 1;
        while (*value == ' ' || *value == '\t') value++;
        size_t length = strcspn(value, "\r\n");
        if (length >= sizeof(reply->location)) return 0;
        memcpy(reply->location, value, length); reply->location[length] = 0;
    }
    if (hop(line) || header_is(line, "Content-Length") || header_is(line, "Set-Cookie") ||
        header_is(line, "Content-Encoding") || header_is(line, "ETag") || header_is(line, "Content-MD5") ||
        !strcmp(line, "\r\n")) return n;
    if (n > HEAD_MAX - reply->headers_len - 1) return 0;
    memcpy(reply->headers + reply->headers_len, line, n);
    reply->headers_len += n;
    return n;
}
/* Every guestfwd connection is a new process. Share the upstream gate so a
 * page's parallel resources cannot keep hammering an already limited archive. */
static int archive_gate(const char *config)
{
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s.archive-gate", config) >= (int)sizeof(path))
        fail(503, "Archive state path is too long");
    int fd = open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
    if (fd < 0 || flock(fd, LOCK_EX)) fail(503, "Archive request coordination unavailable");
    return fd;
}
static void archive_limited(long seconds)
{
    reply_printf(1, "HTTP/1.0 429 Too Many Requests\r\nConnection: close\r\n"
            "Content-Type: text/html; charset=utf-8\r\nRetry-After: %ld\r\n"
            "Cache-Control: no-store\r\n\r\n"
            "<html><head><title>Archive temporarily busy</title></head><body>"
            "<h2>Wayback Machine is temporarily limiting requests</h2>"
            "<p>Light Touch has paused archive requests. Please wait %ld seconds, "
            "then reload this page.</p></body></html>", seconds, seconds);
    client_finish(); exit(0);
}
static void archive_wait(int fd)
{
    long long until = 0;
    if (pread(fd, &until, sizeof(until), 0) != sizeof(until)) until = 0;
    long long now = (long long)time(NULL);
    if (until > now) archive_limited((long)(until - now));
}
static void archive_cooldown(int fd, long seconds)
{
    long long until = (long long)time(NULL) + seconds;
    if (pwrite(fd, &until, sizeof(until), 0) != sizeof(until))
        fail(503, "Archive cooldown could not be saved");
}
/* Archive mode is a read-only HTTP presentation. Keep absolute links in text
 * resources on HTTP so old WebKit does not start an unsupported TLS tunnel.
 * Binary resources are never rewritten. Host-to-archive TLS stays verified. */
static void archive_http_links(ArchiveReply *reply)
{
    if (!reply->textual) return;
    size_t out = 0;
    for (size_t in = 0; in < reply->body_len;) {
        if (reply->body_len - in >= 8 && !strncasecmp(reply->body + in, "https://", 8)) {
            memcpy(reply->body + out, "http://", 7); out += 7; in += 8;
        } else reply->body[out++] = reply->body[in++];
    }
    reply->body_len = out;
}
/* Bounded direct-mapped cache: 64 slots, at most 2 MiB per response, one day.
 * Check the complete date/URL key as well as the hash, so collisions only evict.
 * The shared gate covers reads and writes; no partial response is published. */
#define CACHE_MAX (2 * 1024 * 1024)
static int archive_cache(const char *config, const char *key)
{
    uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char *p = (const unsigned char *)key; *p; p++)
        hash = (hash ^ *p) * 1099511628211ULL;
    char path[PATH_MAX];
    if (snprintf(path, sizeof(path), "%s.archive-cache-%02x", config, (unsigned)(hash % 64)) >= (int)sizeof(path)) return -1;
    return open(path, O_RDWR | O_CREAT | O_NOFOLLOW, 0600);
}
static bool archive_cached(int fd, const char *key)
{
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0 || st.st_size > CACHE_MAX ||
        time(NULL) - st.st_mtime >= 86400) return false;
    char *data = malloc((size_t)st.st_size);
    if (!data) return false;
    size_t key_len = strlen(key);
    bool hit = pread(fd, data, (size_t)st.st_size, 0) == st.st_size &&
        (size_t)st.st_size > key_len + 1 && !memcmp(data, key, key_len) && data[key_len] == '\n';
    if (hit) sendall(1, data + key_len + 1, (size_t)st.st_size - key_len - 1);
    free(data);
    return hit;
}
static void archive_emit(int fd, ArchiveReply *reply, long status, bool head_only)
{
    reply_printf(fd, "HTTP/1.0 %ld Archive response\r\n", status);
    sendall(fd, reply->headers, reply->headers_len);
    if (!head_only) reply_printf(fd, "Content-Length: %zu\r\n", reply->body_len);
    reply_printf(fd, "Connection: close\r\n\r\n");
    if (!head_only) sendall(fd, reply->body, reply->body_len);
}
static void archived_request(const char *target, const char *date, bool head_only, const char *config)
{
    /* Replay original pages over verified host TLS. The guest never needs a
     * modern TLS stack, Python, a separate server, or a host listening port. */
    char url[32768];
    snprintf(url, sizeof(url), "%s/web/%sid_/%s", ARCHIVE_ORIGIN, date, target);
    int gate = archive_gate(config);
    int cache = head_only ? -1 : archive_cache(config, url);
    if (archive_cached(cache, url)) { client_finish(); exit(0); }
    char cache_key[sizeof(url)];
    memcpy(cache_key, url, strlen(url) + 1);
    archive_wait(gate);
    CURL *curl = curl_easy_init();
    ArchiveReply *reply = calloc(1, sizeof(*reply));
    if (!curl || !reply) fail(503, "Archive proxy unavailable");
    curl_easy_setopt(curl, CURLOPT_PROXY, "");
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTP | CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_NOBODY, head_only ? 1L : 0L);
    curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "LightTouch/1.0 (archive replay)");
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, archive_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, reply);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, archive_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, reply);
    long status = 0;
    for (int redirect = 0; redirect < 8; redirect++) {
        reply->headers_len = 0; reply->body_len = 0; reply->location[0] = 0;
        reply->textual = false; reply->retry_after = 60;
        curl_easy_setopt(curl, CURLOPT_URL, url);
        /* One request per second across processes, including redirect hops. */
        struct timespec pace = {1, 0};
        while (nanosleep(&pace, &pace) && errno == EINTR) {}
        CURLcode rc = curl_easy_perform(curl);
        if (rc) fail(502, "The archive could not be reached or its response was too large");
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        if (status == 429 || status == 503) {
            long seconds = reply->retry_after;
            archive_cooldown(gate, seconds);
            archive_limited(seconds);
        }
        const char *location = reply->location;
        if (status >= 300 && status < 400 && location[0]) {
            size_t base = strlen(ARCHIVE_ORIGIN);
            if (!strncmp(location, ARCHIVE_ORIGIN, base) && location[base] == '/') {
                snprintf(url, sizeof(url), "%s", location);
                continue;
            }
            if (location[0] == '/' && location[1] != '/') {
                snprintf(url, sizeof(url), "%s%s", ARCHIVE_ORIGIN, location);
                continue;
            }
        }
        /* An original site's redirect must stay in dated browsing, including
         * HTTP-to-HTTPS redirects. Do not hand Safari a TLS-only destination. */
        if (status >= 300 && status < 400 &&
            (!strncmp(location, "http://", 7) || !strncmp(location, "https://", 8))) {
            snprintf(url, sizeof(url), "%s/web/%sid_/%s", ARCHIVE_ORIGIN, date, location);
            continue;
        }
        archive_http_links(reply);
        if (cache >= 0 && status == 200 &&
            strlen(cache_key) + reply->headers_len + reply->body_len + 256 < CACHE_MAX &&
            ftruncate(cache, 0) == 0 && lseek(cache, 0, SEEK_SET) == 0) {
            reply_printf(cache, "%s\n", cache_key);
            archive_emit(cache, reply, status, false);
        }
        if (cache >= 0) close(cache);
        flock(gate, LOCK_UN); close(gate);
        archive_emit(1, reply, status, head_only);
        curl_easy_cleanup(curl); free(reply->body); free(reply);
        client_finish(); exit(0);
    }
    fail(502, "The archive returned too many redirects");
}

int main(int argc, char **argv)
{
    /* libslirp's command transport merges stderr into the guest socket.
     * No library/diagnostic output may precede an HTTP status line. */
    if (!freopen("/dev/null", "w", stderr)) return 1;
    signal(SIGPIPE, SIG_IGN);
    /* Bounds even a stalled request or host resolver; process owns one stream. */
    alarm(120);
#ifdef HAVE_OPENSSL
    if (argc == 3 && !strcmp(argv[1], "--init-ca")) return tls_initialize(argv[2]);
#endif
    if (argc != 2) fail(503, "Proxy configuration unavailable");
    FILE *config = fopen(argv[1], "r");
    char mode[32] = "", host[256] = "", port[16] = "";
    if (!config) fail(503, "Proxy is disabled");
    int fields = fscanf(config, "%31s %255s %15s", mode, host, port);
    fclose(config);
    if (fields == 3 && !strcmp(mode, "upstream")) {
        char *end; long number = strtol(port, &end, 10);
        if (*end || number < 1 || number > 65535) fail(503, "Invalid proxy port");
        int fd = connect_host(host, port);
        if (fd < 0) fail(502, "Upstream proxy unavailable");
        relay(fd); close(fd); return 0;
    }
    bool archive = !strcmp(mode, "archive");
    if (archive) {
        if (fields != 2 || strlen(host) != 8) fail(503, "Invalid archive date");
        for (int i = 0; i < 8; i++) if (!isdigit((unsigned char)host[i])) fail(503, "Invalid archive date");
    } else if (strcmp(mode, "direct") && strcmp(mode, "off")) fail(503, "Invalid proxy configuration");
    /* Safari can retain a proxy connection while configd applies No Proxy.
     * Stale connections go directly to their origin during that transition. */
    char tunnel[300] = "";
#ifdef HAVE_OPENSSL
read_request:;
#endif
    char head[HEAD_MAX]; size_t n = 0;
    while (n < sizeof(head) - 1) {
        ssize_t k = client_read(head + n, 1);
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k <= 0) fail(400, "Incomplete request");
        if (head[n] == 0) fail(400, "Invalid request byte");
        n++; head[n] = 0;
        if (n >= 4 && !memcmp(head + n - 4, "\r\n\r\n", 4)) break;
    }
    if (n == sizeof(head) - 1) fail(431, "Request headers too large");
    char method[32], target[16384], version[16], extra;
    char *line_end = strstr(head, "\r\n");
    if (!line_end) fail(400, "Invalid request");
    *line_end = 0;
    if (sscanf(head, "%31s %16383s %15s %c", method, target, version, &extra) != 3 ||
        (strcmp(version, "HTTP/1.0") && strcmp(version, "HTTP/1.1"))) fail(400, "Invalid request");
    if (!strcmp(method, "CONNECT")) {
        if (tunnel[0]) fail(400, "Nested TLS tunnels are unsupported");
        if (strlen(target) >= sizeof(tunnel)) fail(400, "Tunnel destination too long");
        strcpy(tunnel, target);
        char *colon = strrchr(target, ':');
        if (!colon || colon == target) fail(400, "Invalid tunnel destination");
        *colon++ = 0; char *end; long number = strtol(colon, &end, 10);
        if (*end || number < 1 || number > 65535) fail(400, "Invalid tunnel port");
        char *hostname = target;
        size_t length = strlen(hostname);
        if (length > 2 && hostname[0] == '[' && hostname[length - 1] == ']') {
            hostname[length - 1] = 0; hostname++;
        }
        if (!strcmp(mode, "direct") && retired_host(hostname))
            fail(410, "This online service has been retired");
#ifdef HAVE_OPENSSL
        char ca_path[PATH_MAX];
        if (strcmp(mode, "off") && tls_path(ca_path, sizeof(ca_path), argv[1], ".ca.pem") && access(ca_path, F_OK) == 0) {
            SSL_CTX *ctx = tls_server_context(argv[1], hostname);
            if (!ctx) fail(503, "Local TLS certificate unavailable");
            const char ok[] = "HTTP/1.0 200 Connection established\r\n\r\n";
            if (!sendall(1, ok, sizeof(ok) - 1)) return 1;
            fcntl(0, F_SETFL, fcntl(0, F_GETFL) & ~O_NONBLOCK);
            fcntl(1, F_SETFL, fcntl(1, F_GETFL) & ~O_NONBLOCK);
            client_tls = SSL_new(ctx); SSL_CTX_free(ctx);
            if (!client_tls || !SSL_set_rfd(client_tls, 0) || !SSL_set_wfd(client_tls, 1) || SSL_accept(client_tls) != 1) return 1;
            goto read_request;
        }
#endif
        if (archive) fail(405, "Dated HTTPS needs the local TLS bridge");
        int fd = connect_host(hostname, colon);
        if (fd < 0) fail(502, "Destination unavailable");
        const char ok[] = "HTTP/1.0 200 Connection established\r\n\r\n";
        if (sendall(1, ok, sizeof(ok) - 1)) relay(fd);
        close(fd); return 0;
    }
    if (archive && strcmp(method, "GET") && strcmp(method, "HEAD"))
        fail(405, "Archive browsing supports HTTP GET and HEAD");
    if (tunnel[0]) {
        if (target[0] != '/') fail(400, "TLS request needs an origin-form path");
        char absolute[sizeof(target)];
        int length = snprintf(absolute, sizeof(absolute), "https://%s%s", tunnel, target);
        if (length < 0 || length >= (int)sizeof(absolute)) fail(414, "URL too long");
        strcpy(target, absolute);
    }
    for (const char *p = method; *p; p++) if (!isupper((unsigned char)*p)) fail(400, "Invalid method");
    if (!tunnel[0] && strncmp(target, "http://", 7)) fail(400, "An absolute HTTP URL is required");
    if (!strcmp(mode, "direct") && retired_url(target))
        fail(410, "This online service has been retired");
    if (archive) {
        const char *authority = target + 7;
        const char *end = strchr(authority, '/');
        const char *at = strchr(authority, '@');
        if (at && (!end || at < end)) fail(400, "Archive URLs cannot contain credentials");
        archived_request(target, host, !strcmp(method, "HEAD"), argv[1]);
    }
    struct curl_slist *headers = NULL;
    size_t body_length = 0; bool seen_length = false;
    for (char *p = line_end + 2; *p && strcmp(p, "\r\n"); ) {
        char *end = strstr(p, "\r\n");
        if (!end) fail(400, "Invalid header");
        *end = 0;
        if (*p == ' ' || *p == '\t' || !strchr(p, ':')) fail(400, "Invalid header");
        if (header_is(p, "Transfer-Encoding")) fail(501, "Chunked request bodies are unsupported");
        if (header_is(p, "Content-Length")) {
            char *tail; const char *value = strchr(p, ':') + 1;
            while (*value == ' ') value++;
            if (seen_length || !isdigit((unsigned char)*value)) fail(400, "Invalid content length");
            unsigned long length = strtoul(value, &tail, 10);
            if (*tail || length > BODY_MAX) fail(413, "Invalid or oversized body");
            body_length = length; seen_length = true;
        } else if (header_is(p, "Expect")) {
            fail(417, "Expect is unsupported");
        } else if (!hop(p) && !header_is(p, "Host")) {
            headers = curl_slist_append(headers, p);
        }
        p = end + 2;
    }
    char *body = malloc(body_length + 1);
    if (!body) fail(503, "Out of memory");
    for (size_t read_bytes = 0; read_bytes < body_length;) {
        ssize_t k = client_read(body + read_bytes, body_length - read_bytes);
        if (k < 0 && (errno == EINTR || errno == EAGAIN)) continue;
        if (k <= 0) fail(400, "Incomplete body");
        read_bytes += k;
    }
    CURL *curl = curl_easy_init();
    if (!curl) fail(503, "HTTP unavailable");
    curl_easy_setopt(curl, CURLOPT_URL, target);
    curl_easy_setopt(curl, CURLOPT_PROXY, ""); /* Never inherit host proxy credentials/settings. */
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, tunnel[0] ? (CURLPROTO_HTTP | CURLPROTO_HTTPS) : CURLPROTO_HTTP);
    curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_0);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 60L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (!strcmp(method, "HEAD")) curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    else if (seen_length) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)body_length);
    }
    headers = curl_slist_append(headers, "Connection: close");
    headers = curl_slist_append(headers, "Expect:");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    bool started = false;
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &started);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, response_header);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, output);
    CURLcode rc = curl_easy_perform(curl);
    if (rc) fprintf(stderr, "itwebproxy: %s\n", curl_easy_strerror(rc));
    curl_easy_cleanup(curl); curl_slist_free_all(headers); free(body);
    if (rc && !started) fail(502, "Destination unavailable");
    client_finish();
    return rc ? 1 : 0;
}
