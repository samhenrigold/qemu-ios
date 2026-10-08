/*
 * The GL bridge's name table, include/hw/arm/guest-services/gles-names.h, checked on the host:
 *
 *   cc -o gligen contrib/gles-public/gligen.c -lz && ./gligen ROOT          check: ids, argc, flags, the stamp,
 *                                                                          the raw ids the host and the shim use
 *   ./gligen ROOT --stamp                                                  print the stamp a hand edit needs
 *
 * contrib/gles-public/gligen.sh builds and runs it. Both the guest shims (contrib/it-gles/mbxshim.c,
 * contrib/gles-public/opengles.c) and the host (gles.h, gles-host.c) include the header, so the wire ids cannot drift
 * between them. A row is GLES_FN(name, dispatch_field, id, argc, flags). Ids are append-only: the ones below 822 are
 * the 3.1.3 dispatch slots the host has always decoded, the rest were assigned from 822 up. GLES_NAMES_VERSION is the
 * CRC-32 of the rows (`name field id argc flags`, argc None for NA, joined by newlines), carried in the shim's hello.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define NA (-1)
#define GLES_OP_BASE 0x1000
static const struct row { const char *name, *field; int id, argc, flags; } rows[] = {
#define GLES_FN(name, field, id, argc, flags) { #name, #field, id, argc, flags },
#include "../../include/hw/arm/guest-services/gles-names.h"
#undef GLES_FN
};
#define NROWS (sizeof(rows) / sizeof(rows[0]))

static int failures;
#define CHECK(cond, ...) do { if (!(cond)) { fprintf(stderr, "gligen: " __VA_ARGS__); fputc('\n', stderr); failures++; } } while (0)

static unsigned long stamp(void)
{
    uLong crc = crc32(0, Z_NULL, 0);
    char line[512];
    for (size_t i = 0; i < NROWS; i++) {
        char argc[16];
        snprintf(argc, sizeof(argc), rows[i].argc == NA ? "None" : "%d", rows[i].argc);
        int n = snprintf(line, sizeof(line), "%s%s %s %d %s %d", i ? "\n" : "", rows[i].name, rows[i].field,
                         rows[i].id, argc, rows[i].flags);
        crc = crc32(crc, (const Bytef *)line, (uInt)n);
    }
    return crc & 0xffffffffUL;
}

static const struct row *by_name(const char *name)
{
    for (size_t i = 0; i < NROWS; i++) {
        if (!strcmp(rows[i].name, name)) {
            return &rows[i];
        }
    }
    return NULL;
}

static int has_id(int id)
{
    for (size_t i = 0; i < NROWS; i++) {
        if (rows[i].id == id) {
            return 1;
        }
    }
    return 0;
}

static char *slurp(const char *root, const char *rel)
{
    char path[4096];
    snprintf(path, sizeof(path), "%s/%s", root, rel);
    FILE *f = fopen(path, "rb");
    if (!f) {
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    char *text = malloc((size_t)n + 1);
    text[fread(text, 1, (size_t)n, f)] = 0;
    fclose(f);
    return text;
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: gligen ROOT [--stamp]\n");
        return 2;
    }
    if (argc == 3 && !strcmp(argv[2], "--stamp")) {
        printf("0x%08lx\n", stamp());
        return 0;
    }
    int max = 0;
    for (size_t i = 0; i < NROWS; i++) {
        for (size_t j = 0; j < i; j++) {
            CHECK(strcmp(rows[i].name, rows[j].name), "duplicate name %s", rows[i].name);
            CHECK(strcmp(rows[i].field, rows[j].field), "duplicate dispatch field %s", rows[i].field);
            CHECK(rows[i].id != rows[j].id, "duplicate id %d", rows[i].id);
        }
        CHECK(rows[i].id >= 0 && rows[i].id < GLES_OP_BASE, "%s: id %d out of range", rows[i].name, rows[i].id);
        CHECK(rows[i].argc == NA || (rows[i].argc >= 0 && rows[i].argc <= 12), "%s: argc", rows[i].name);
        CHECK(!(rows[i].flags & GLES_F_BATCH) || rows[i].argc != NA, "%s: batched but not forwardable", rows[i].name);
        max = rows[i].id > max ? rows[i].id : max;
    }
    CHECK(GLES_NAMES_VERSION == stamp(), "GLES_NAMES_VERSION is stale: set it to 0x%08lx (gligen.sh --stamp)", stamp());
    CHECK(GLES_ID_MAX == max, "GLES_ID_MAX is %d, the largest id %d", GLES_ID_MAX, max);
    /* Known ES1 wire numbers the host has always decoded (gles.h). */
    static const struct { const char *name; int id, argc; } wire[] = {
        { "glOrthof", 791, 6 }, { "glAlphaFuncx", 761, -2 }, { "glClearDepthf", 763, -2 }, { "glTexImage2D", 301, 9 },
        { "glDrawArrays", -2, 3 }, { "glClear", -2, 1 }, { "glUseProgram", 600, -2 }, { "glUniformMatrix2fv", -2, 4 },
        { "glVertexAttribPointer", -2, 6 }, { "glBindFramebuffer", 672, -2 },
    };
    for (size_t i = 0; i < sizeof(wire) / sizeof(wire[0]); i++) {
        const struct row *r = by_name(wire[i].name);
        CHECK(r && (wire[i].id == -2 || r->id == wire[i].id) && (wire[i].argc == -2 || r->argc == wire[i].argc),
              "%s: wire id/argc changed", wire[i].name);
    }
    static const char *batched[] = { "glEnable", "glBlendFunc", "glBindTexture", "glUniform4f", "glViewport",
                                     "glTranslatef", "glClearColor", "glUseProgram" };
    static const char *unbatched[] = { "glDrawArrays", "glDrawElements", "glFlush", "glFinish", "glGetError",
                                       "glTexImage2D", "glUniform4fv", "glVertexPointer", "glReadPixels",
                                       "glIsEnabled", "glLoadMatrixx", "glGenTextures", "glCreateShader" };
    for (size_t i = 0; i < sizeof(batched) / sizeof(batched[0]); i++) {
        const struct row *r = by_name(batched[i]);
        CHECK(r && (r->flags & GLES_F_BATCH), "%s must be GLES_F_BATCH", batched[i]);
    }
    for (size_t i = 0; i < sizeof(unbatched) / sizeof(unbatched[0]); i++) {
        const struct row *r = by_name(unbatched[i]);
        CHECK(r && !(r->flags & GLES_F_BATCH), "%s must not be GLES_F_BATCH", unbatched[i]);
    }
    /* Every raw id the host's ES2 switch (absent in a guest-tools source copy) and the shim's hand thunks name. */
    char *host = slurp(argv[1], "hw/arm/gles-host.c");
    if (host) {
        char *p = strstr(host, "static bool gles_es2_call(");
        CHECK(p, "gles-host.c has no gles_es2_call");
        char *end = p ? strstr(p, "\n}\n") : NULL;
        for (; p && p < end && (p = strstr(p, "case ")) && p < end; p += 5) {
            int a, b;
            char tail[8];
            if (sscanf(p, "case %d ... %d%1[:]", &a, &b, tail) == 3) {
            } else if (sscanf(p, "case %d%1[:]", &a, tail) == 2) {
                b = a;
            } else {
                continue;
            }
            for (int id = a; id <= b; id++) {
                CHECK(has_id(id), "gles-host.c uses id %d, which is not in the table", id);
            }
        }
    }
    char *shim = slurp(argv[1], "contrib/it-gles/mbxshim.c");
    CHECK(shim, "no contrib/it-gles/mbxshim.c");
    for (char *p = shim; p && (p = strstr(p, "qc(")); p += 3) {
        int id;
        char comma[2];
        if ((p == shim || !(p[-1] == '_' || (p[-1] >= 'a' && p[-1] <= 'z') || (p[-1] >= 'A' && p[-1] <= 'Z') ||
                             (p[-1] >= '0' && p[-1] <= '9'))) &&
            sscanf(p, "qc(%d%1[,]", &id, comma) == 2) {
            CHECK(has_id(id), "mbxshim.c uses id %d, which is not in the table", id);
        }
    }
    /* The 3.1.3 trampoline map (tests read it) agrees with the ids. 3.1.3 exports the OES spelling, and
     * glDeleteProgram/glDeleteShader share one slot. */
    char *map = slurp(argv[1], "contrib/it-gles/slotmap.txt");
    CHECK(map, "no contrib/it-gles/slotmap.txt");
    for (char *line = map ? strtok(map, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        int slot;
        char name[128];
        if (sscanf(line, "%d %127s", &slot, name) != 2) {
            continue;
        }
        const struct row *r = by_name(name);
        for (const char *suffix[] = { "OES", "EXT", "APPLE" }, **s = suffix; !r && s < suffix + 3; s++) {
            size_t n = strlen(name), k = strlen(*s);
            if (n > k && !strcmp(name + n - k, *s)) {
                char base[128];
                snprintf(base, sizeof(base), "%.*s", (int)(n - k), name);
                r = by_name(base);
            }
        }
        CHECK(has_id(slot) && (!r || r->id == slot), "%s: slotmap says %d", name, slot);
    }
    if (failures) {
        return 1;
    }
    size_t forwardable = 0;
    for (size_t i = 0; i < NROWS; i++) {
        forwardable += rows[i].argc != NA;
    }
    printf("gligen: %zu functions, %zu forwardable, version %08lx\n", NROWS, forwardable, stamp());
    return 0;
}
