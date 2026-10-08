/* Inject legacy SQLite schema changes into the production read-only query.
 *
 * Native media QA exercises the actual service. This fault test makes the rare
 * schema race deterministic and verifies that unrelated errors remain fatal:
 * mode 0 (schema change on the first connection) reopens and succeeds, mode 1 (persistent schema change)
 * and mode 2 (busy) fail, at each of the ATTACH/prepare/step phases. Each case runs in a child.
 *
 * SLICE contrib/it-media/itmedia.c fn read_query
 */
#define _DEFAULT_SOURCE
#include <string.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sqlite3.h>
#define LIBRARY "unused/"
#define ML3_LIBRARY "unused.sqlitedb"
static int ml3;   /* 0: the legacy Library.itdb + Locations.itdb path under test */
static int connections, finalizations, steps, mode, failure_phase;
static void fail(const char *reason) { fprintf(stderr,"%s\n",reason); exit(1); }
static void database_failure(sqlite3 *db,const char *reason) {
    assert(connections == (mode == 1 ? 3 : 1)); fail(reason);
}
static int sql_open(const char *path,sqlite3 **db,int flags,const char *vfs) {
    assert(flags == SQLITE_OPEN_READONLY); *db=(sqlite3 *)1; connections++; return SQLITE_OK;
}
static int sql_timeout(sqlite3 *db,int ms) { assert(ms==5000); return SQLITE_OK; }
static int injected(int phase) {
    if (phase != failure_phase) return SQLITE_OK;
    if (mode == 2) return SQLITE_BUSY;
    return mode == 1 || connections == 1 ? SQLITE_SCHEMA : SQLITE_OK;
}
static int sql_exec(sqlite3 *db,const char *s,void *a,void *b,void *c) { return injected(0); }
static int sql_prepare(sqlite3 *db,const char *s,int len,sqlite3_stmt **stmt,const char **tail) {
    int rc=injected(1); *stmt=rc==SQLITE_OK ? (sqlite3_stmt *)1 : NULL; return rc;
}
static int sql_bind(sqlite3_stmt *stmt,int index,const char *s,int len,void (*destroy)(void *)) {
    assert(stmt && index>=1 && index<=2 && destroy==SQLITE_TRANSIENT); return SQLITE_OK;
}
static int sql_step(sqlite3_stmt *stmt) {
    assert(stmt); steps++; int rc=injected(2); return rc==SQLITE_OK ? SQLITE_ROW : rc;
}
static sqlite3_int64 sql_column(sqlite3_stmt *stmt,int index) { assert(stmt && index==0); return 42; }
static int sql_finalize(sqlite3_stmt *stmt) { finalizations++; return SQLITE_OK; }
static int sql_close(sqlite3 *db) { assert(db); return SQLITE_OK; }
#include "slice.h"
static void one(void) {
    sqlite3_int64 value=0;
    int rc=read_query("read only fixture","folder","file",&value,NULL);
    assert(mode==0 && rc==SQLITE_ROW && value==42);
    assert(connections==2 && finalizations==2);
    assert(steps==(failure_phase==2 ? 2 : 1));
}
int main(void) {
    for (failure_phase=0; failure_phase<3; failure_phase++)
        for (mode=0; mode<3; mode++) {
            fflush(stdout);
            pid_t pid=fork(); assert(pid>=0);
            if (!pid) { int null=open("/dev/null",O_WRONLY); dup2(null,2); one(); exit(0); }
            int status; assert(waitpid(pid,&status,0)==pid);
            assert(WIFEXITED(status) && WEXITSTATUS(status)==(mode==0 ? 0 : 1));
        }
    puts("PASS: ATTACH/prepare/step schema retry, bounded persistent schema failure, fatal busy error");
}
