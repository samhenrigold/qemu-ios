/*
 * Guest-package delivery, shared by the iPod Touch 2G and iPad 1 machines.
 * See include/hw/arm/guest-package.h. The core (guest_pkg_op and the report
 * state) needs only glib, so tests/guest-package/test_guest_package.py builds
 * it on the host with GUEST_PKG_CORE_ONLY.
 */
#include "qemu/osdep.h"
#ifndef GUEST_PKG_CORE_ONLY
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "exec/cpu-common.h"
#include "hw/core/cpu.h"
#endif
#include "hw/arm/guest-package.h"

/* One machine per process; the app's helper reads this from its own thread. */
static GMutex last_lock;
static struct {
    bool reported;
    int64_t serial;
    int32_t result;
    uint64_t reports;
    char text[GUEST_PKG_WINDOW + 1];
    bool hello;
    int32_t proto;
    int64_t gl_serial;
} last;

static void drop_snapshot(GuestPackage *p)
{
    g_clear_pointer(&p->offer, g_free);
    p->offer_len = 0;
    for (int i = 0; i < GUEST_PKG_ENTRIES; i++) {
        g_clear_pointer(&p->paths[i], g_free);
    }
    g_clear_pointer(&p->file, g_free);
    p->file_len = 0;
    p->file_idx = -1;
}

void guest_pkg_set_dir(GuestPackage *p, const char *dir)
{
    drop_snapshot(p);
    g_free(p->dir);
    p->dir = dir && *dir ? g_strdup(dir) : NULL;
}

void guest_pkg_reset(GuestPackage *p)
{
    drop_snapshot(p);
    g_mutex_lock(&last_lock);
    memset(&last, 0, sizeof(last));
    g_mutex_unlock(&last_lock);
}

static bool safe_rel(const char *path)
{
    return *path && *path != '/' && !strstr(path, "..") && !strchr(path, '\\');
}

/* Read DIR/offer and map each payload line's index to its path. */
static int64_t snapshot(GuestPackage *p)
{
    g_autofree char *path = NULL;
    gsize len;

    drop_snapshot(p);
    if (!p->dir) {
        return -1;
    }
    path = g_build_filename(p->dir, "offer", NULL);
    if (!g_file_get_contents(path, &p->offer, &len, NULL) ||
        len == 0 || len >= GUEST_PKG_OFFER_MAX) {
        drop_snapshot(p);
        return -1;
    }
    p->offer_len = len;
    g_auto(GStrv) lines = g_strsplit(p->offer, "\n", -1);
    for (char **l = lines; *l; l++) {
        g_auto(GStrv) f = g_strsplit_set(*l, " \t", -1);
        char *tok[3];
        int n = 0;
        for (char **t = f; *t && n < 3; t++) {
            if (**t) {
                tok[n++] = *t;
            }
        }
        if (n < 3 || (strcmp(tok[0], "file") && strcmp(tok[0], "job") &&
                      strcmp(tok[0], "hook"))) {
            continue;
        }
        char *end;
        unsigned long idx = strtoul(tok[1], &end, 10);
        if (*end || idx >= GUEST_PKG_ENTRIES || p->paths[idx] ||
            !safe_rel(tok[2])) {
            continue;       /* the guest rejects the offer line itself */
        }
        p->paths[idx] = g_build_filename(p->dir, tok[2], NULL);
    }
    return len;
}

static int64_t window(const char *data, size_t size, uint32_t address,
                      uint32_t offset, uint32_t length,
                      GuestPkgCopy copy, void *opaque)
{
    if (length > GUEST_PKG_WINDOW) {
        return -1;
    }
    if (offset >= size) {
        return 0;
    }
    size_t n = MIN((size_t)length, size - offset);
    if (copy(opaque, address, (uint8_t *)data + offset, n, true)) {
        return -1;
    }
    return n;
}

int64_t guest_pkg_op(GuestPackage *p, unsigned op, uint64_t token,
                     uint32_t address, uint32_t offset, uint32_t length,
                     GuestPkgCopy copy, void *opaque)
{
    switch (op) {
    case QC_PKG_OFFER:
        if (length == 0 || !p->offer) {
            int64_t n = snapshot(p);
            if (length == 0 || n < 0) {
                return n;
            }
        }
        return window(p->offer, p->offer_len, address, offset, length,
                      copy, opaque);
    case QC_PKG_READ:
        if (!p->offer || token >= GUEST_PKG_ENTRIES || !p->paths[token]) {
            return -1;
        }
        if (p->file_idx != (int)token) {
            gsize len;
            g_clear_pointer(&p->file, g_free);
            p->file_idx = -1;
            if (!g_file_get_contents(p->paths[token], &p->file, &len, NULL) ||
                len > GUEST_PKG_FILE_MAX) {
                g_clear_pointer(&p->file, g_free);
                return -1;
            }
            p->file_len = len;
            p->file_idx = token;
        }
        if (length == 0) {
            return p->file_len;
        }
        return window(p->file, p->file_len, address, offset, length,
                      copy, opaque);
    case QC_PKG_REPORT: {
        char text[GUEST_PKG_WINDOW + 1] = "";
        if (length > GUEST_PKG_WINDOW ||
            (length && copy(opaque, address, (uint8_t *)text, length, false))) {
            return -1;
        }
        text[length] = 0;
        for (char *c = text; *c; c++) {
            if (!g_ascii_isprint(*c)) {
                *c = '?';
            }
        }
        g_mutex_lock(&last_lock);
        last.reported = true;
        last.serial = token;
        last.result = (int32_t)offset;
        last.reports++;
        memcpy(last.text, text, sizeof(text));
        g_mutex_unlock(&last_lock);
        return 0;
    }
    case QC_GLES_HELLO:
        g_mutex_lock(&last_lock);
        last.hello = true;
        last.proto = (int32_t)offset;
        last.gl_serial = token;
        g_mutex_unlock(&last_lock);
        return GUEST_GLES_PROTO;
    }
    return -1;
}

bool guest_pkg_last_report(int64_t *serial, int32_t *result)
{
    g_mutex_lock(&last_lock);
    bool have = last.reported;
    *serial = last.serial;
    *result = last.result;
    g_mutex_unlock(&last_lock);
    return have;
}

int32_t guest_pkg_gles_protocol(int64_t *serial)
{
    g_mutex_lock(&last_lock);
    int32_t proto = last.hello ? last.proto : 0;
    if (serial) {
        *serial = last.hello ? last.gl_serial : 0;
    }
    g_mutex_unlock(&last_lock);
    return proto;
}

char *guest_pkg_status(void)
{
    g_mutex_lock(&last_lock);
    char *s = last.reported
        ? g_strdup_printf("report %" PRId64 " %d (%" PRIu64 " this boot) %s",
                          last.serial, last.result, last.reports, last.text)
        : g_strdup("no report");
    char *out = last.hello
        ? g_strdup_printf("%s; gles-hello proto %d serial %" PRId64, s,
                          last.proto, last.gl_serial)
        : g_strdup_printf("%s; no gles-hello (proto 0)", s);
    g_mutex_unlock(&last_lock);
    g_free(s);
    return out;
}

#ifndef GUEST_PKG_CORE_ONLY
static int pkg_copy(void *opaque, uint32_t address, uint8_t *data, size_t len,
                    bool write)
{
    if (len && len - 1 > UINT32_MAX - address) {
        return -1;
    }
    return cpu_memory_rw_debug(opaque, address, data, len, write);
}

bool guest_pkg_call(GuestPackage *p, CPUState *cpu, qemu_call_t *q,
                    int32_t *err)
{
    switch (q->call_number) {
    case QC_GLES_HELLO:
    case QC_PKG_OFFER:
    case QC_PKG_READ:
    case QC_PKG_REPORT:
        break;
    default:
        return false;
    }
    q->retval = guest_pkg_op(p, q->call_number, q->args.ag.token,
                             q->args.ag.buffer_guest_ptr, q->args.ag.offset,
                             q->args.ag.length, pkg_copy, cpu);
    *err = q->retval < 0 ? EINVAL : 0;
    return true;
}

static void pkg_dir_get(Object *obj, Visitor *v, const char *name,
                        void *opaque, Error **errp)
{
    GuestPackage *p = opaque;
    g_autofree char *value = g_strdup(p->dir ? p->dir : "");
    visit_type_str(v, name, &value, errp);
}

static void pkg_dir_set(Object *obj, Visitor *v, const char *name,
                        void *opaque, Error **errp)
{
    g_autofree char *value = NULL;
    if (visit_type_str(v, name, &value, errp)) {
        guest_pkg_set_dir(opaque, value);
    }
}

static void pkg_status_get(Object *obj, Visitor *v, const char *name,
                           void *opaque, Error **errp)
{
    g_autofree char *value = guest_pkg_status();
    visit_type_str(v, name, &value, errp);
}

void guest_pkg_init(GuestPackage *p, Object *machine)
{
    p->file_idx = -1;
    object_property_add(machine, "guest-package", "string", pkg_dir_get,
                        pkg_dir_set, NULL, p);
    object_property_set_description(machine, "guest-package",
        "Directory holding this boot's guest-package offer (an 'offer' file "
        "and the payloads it names), served to the guest loader it_boot over "
        "QC_PKG_*. Empty: no offer, and the guest keeps its current package");
    object_property_add(machine, "guest-package-status", "string",
                        pkg_status_get, NULL, NULL, p);
    object_property_set_description(machine, "guest-package-status",
        "it_boot's last report this boot (serial, result code, its status "
        "line) and the GL shim's QC_GLES_HELLO, if any");
}
#endif
