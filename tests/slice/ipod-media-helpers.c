/* Offline checks for media identity queries and photo receipt reconciliation.
 *
 * The photo harness compiles the production receipt-handling code and stops
 * before loading Foundation or submitting a save: itphoto's helpers and main's full filesystem preflight,
 * with only the ARM entrypoint, privilege setup (getuid() is mobile) and native API phase replaced, and
 * /var/mobile/Media moved to a local Media directory. All files are local fixtures.
 * The identity half runs existing()'s two location queries, `ml3 ? 5.x's : the itlp library's`, against each
 * schema in an in-memory SQLite.
 *
 * SLICE:photo-head contrib/it-media/itphoto.c range /* A bounded native | #define ROOT
 * SLICE:photo contrib/it-media/itphoto.c range typedef void *ID; | __attribute__((naked))
 * SLICE:photo-main contrib/it-media/itphoto.c range int main(int argc, char **argv) { | void *objc = dlopen
 * SLICE:sql contrib/it-media/itmedia.c range const char *sql = ml3 | ;
 * CFLAGS -lsqlite3
 */
#include <assert.h>
#include <sqlite3.h>
#include <sys/wait.h>
#include "photo-head.h"
#define ROOT "Media/LightTouch"
#define DCIM "Media/DCIM"
#include "photo.h"
#define getuid() 501
#define main itphoto_main
#include "photo-main.h"
    puts("ready-to-save"); return 0;
}
#undef main
#undef getuid

#define STAGING "Media/LightTouch/photo"
#define ALBUM "Media/DCIM/100APPLE"
#define ASSET ALBUM "/IMG_0001.JPG"
#define IMAGE STAGING "/image.jpg"
#define RECEIPT STAGING "/.photo-receipt"
static const char done[] = "done\n" ASSET "\n";

static void put(const char *path, const void *data, size_t len)
{
    FILE *f = fopen(path, "wb");
    assert(f && fwrite(data, 1, len, f) == len && !fclose(f));
}
static size_t get(const char *path, char *buf, size_t cap)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    size_t n = fread(buf, 1, cap, f);
    fclose(f);
    return n;
}
static bool exists(const char *path) { struct stat st; return !lstat(path, &st); }
static bool is_file(const char *path) { struct stat st; return !lstat(path, &st) && S_ISREG(st.st_mode); }

static void setup(void)
{
    assert(!system("rm -rf Media && mkdir -p " STAGING " " ALBUM));
    put(ASSET, "native original", 15);
    /* A bounded baseline JPEG header accepted by the production preflight. Native decoding is outside this harness. */
    static const unsigned char jpeg[] = {0xff,0xd8,0xff,0xc0,0x00,0x0b,0x08,0x01,0xe0,0x02,0x80,0x01,0x01,0x11,0x00};
    put(IMAGE, jpeg, sizeof(jpeg));
}

static char out[4096], err[4096];
static size_t drain(int fd, char *buf)
{
    size_t got = 0;
    ssize_t r;
    while ((r = read(fd, buf + got, 4095 - got)) > 0) got += r;
    buf[got] = 0;
    close(fd);
    return got;
}
/* Run itphoto photo in a child (it leaves through _exit); its exit status, stdout and stderr. */
static int run_helper(void)
{
    int o[2], e[2];
    assert(!pipe(o) && !pipe(e));
    fflush(stdout);
    pid_t pid = fork();
    assert(pid >= 0);
    if (!pid) {
        dup2(o[1], 1); dup2(e[1], 2);
        char *argv[] = {"itphoto", "photo", NULL};
        exit(itphoto_main(2, argv));
    }
    close(o[1]); close(e[1]);
    drain(o[0], out); drain(e[0], err);
    int status;
    assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status));
    return WEXITSTATUS(status);
}
static void assert_refused(const char *content, size_t len)
{
    char now[512];
    put(RECEIPT, content, len);
    assert(run_helper() != 0 && strstr(err, "itphoto:"));
    assert(get(RECEIPT, now, sizeof(now)) == len && !memcmp(now, content, len));
    assert(is_file(IMAGE));
}

static void photo_receipts(void)
{
    char now[512];
    /* A new import reaches the native preflight without a receipt. */
    setup();
    assert(run_helper() == 0 && !strcmp(out, "ready-to-save\n"));
    assert(!exists(RECEIPT) && exists(IMAGE));

    /* Completed and legacy receipts remove redundant staging. */
    setup();
    const char *completed[] = {"done\n", done};
    for (int i = 0; i < 2; i++) {
        size_t len = strlen(completed[i]);
        put(IMAGE, "restaged JPEG", 13);
        put(RECEIPT, completed[i], len);
        assert(run_helper() == 0 && !strcmp(out, "already-imported\n"));
        assert(get(RECEIPT, now, sizeof(now)) == len && !memcmp(now, completed[i], len));
        assert(!exists(IMAGE));
    }

    /* A deleted original or album allows reimport. */
    setup();
    assert(!unlink(ASSET));
    for (int missing_album = 0; missing_album < 2; missing_album++) {
        if (missing_album) assert(!rmdir(ALBUM));
        put(RECEIPT, done, strlen(done));
        assert(run_helper() == 0 && !strcmp(out, "ready-to-save\n"));
        assert(!exists(RECEIPT) && exists(IMAGE));
    }

    /* Uncertain and malformed receipts remain unchanged. */
    char extra[128], wrong[128], nul[128];
    snprintf(extra, sizeof(extra), "%sextra\n", done);
    snprintf(wrong, sizeof(wrong), "done\n" ALBUM "/IMG_000X.JPG\n");
    size_t nul_len = (size_t)snprintf(nul, sizeof(nul), "done\n" ALBUM "/IMG_00?1.JPG\n");
    *strchr(nul, '?') = 0;
    struct { const char *s; size_t len; } bad[] = {
        {"pending\n", 8}, {"", 0}, {"done\ninvalid\n", 13}, {done, strlen(done) - 1},
        {extra, strlen(extra)}, {wrong, strlen(wrong)}, {nul, nul_len},
    };
    for (unsigned i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        setup();
        assert_refused(bad[i].s, bad[i].len);
    }

    /* A missing DCIM does not discard a completed receipt. */
    setup();
    assert(!system("rm -rf Media/DCIM"));
    assert_refused(done, strlen(done));

    /* An invalid album does not discard a completed receipt. */
    setup();
    assert(!system("rm -rf " ALBUM));
    put(ALBUM, "not a directory", 15);
    assert_refused(done, strlen(done));

    /* An invalid original does not discard a completed receipt. */
    setup();
    assert(!unlink(ASSET) && !mkdir(ASSET, 0755));
    assert_refused(done, strlen(done));
}

static int ml3;
static const char *query(int which)
{
    ml3 = which;
#include "sql.h"
    ;
    return sql;
}
/* The query's rows as "a,b;" pairs. */
static const char *rows(sqlite3 *db, const char *folder, const char *file)
{
    static char text[256];
    sqlite3_stmt *stmt;
    assert(sqlite3_prepare_v2(db, query(ml3), -1, &stmt, NULL) == SQLITE_OK);
    sqlite3_bind_text(stmt, 1, folder, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 2, file, -1, SQLITE_TRANSIENT);
    text[0] = 0;
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW)
        snprintf(text + strlen(text), sizeof(text) - strlen(text), "%lld,%lld;",
                 sqlite3_column_int64(stmt, 0), sqlite3_column_int64(stmt, 1));
    assert(rc == SQLITE_DONE);
    sqlite3_finalize(stmt);
    return text;
}
static void media_identity(void)
{
    sqlite3 *db;
    /* The itlp library's location query recognizes songs and movies. */
    ml3 = 0;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "CREATE TABLE item(pid INTEGER, is_song INTEGER, artwork_cache_id INTEGER);"
        "INSERT INTO item VALUES(1,1,23),(2,0,0);"
        "ATTACH DATABASE ':memory:' AS loc;"
        "CREATE TABLE loc.base_location(id INTEGER, path TEXT);"
        "CREATE TABLE loc.location(item_pid INTEGER, base_location_id INTEGER, location TEXT);"
        "INSERT INTO loc.base_location VALUES(1,'LightTouch/song'),(2,'LightTouch/movie');"
        "INSERT INTO loc.location VALUES(1,1,'audio.m4a'),(2,2,'video.mp4');", NULL, NULL, NULL) == SQLITE_OK);
    assert(!strcmp(rows(db, "LightTouch/song", "audio.m4a"), "1,23;"));
    assert(!strcmp(rows(db, "LightTouch/movie", "video.mp4"), "2,0;"));
    assert(!strcmp(rows(db, "LightTouch/song", "video.mp4"), ""));
    assert(!strcmp(rows(db, "LightTouch/other", "audio.m4a"), ""));
    sqlite3_close(db);
    /* 5.x's ml3 location query. */
    ml3 = 1;
    assert(sqlite3_open(":memory:", &db) == SQLITE_OK);
    assert(sqlite3_exec(db,
        "CREATE TABLE item(item_pid INTEGER, base_location_id INTEGER);"
        "CREATE TABLE item_extra(item_pid INTEGER, location TEXT, artwork_cache_id INTEGER);"
        "CREATE TABLE base_location(base_location_id INTEGER, path TEXT);"
        "INSERT INTO item VALUES(-7,5),(9,6);"
        "INSERT INTO item_extra VALUES(-7,'audio.mp3',-7),(9,'audio.mp3',0);"
        "INSERT INTO base_location VALUES(5,'LightTouch/a'),(6,'LightTouch/b');", NULL, NULL, NULL) == SQLITE_OK);
    assert(!strcmp(rows(db, "LightTouch/a", "audio.mp3"), "-7,-7;"));
    assert(!strcmp(rows(db, "LightTouch/b", "audio.mp3"), "9,0;"));
    assert(!strcmp(rows(db, "LightTouch/a", "audio.m4a"), ""));
    sqlite3_close(db);
}

int main(void)
{
    photo_receipts();
    media_identity();
    puts("PASS: photo receipt reconciliation and both media identity queries");
}
