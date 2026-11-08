/* Native draws release the BQL but retain exclusive renderer ownership.
 * Another renderer caller must wait without blocking I/O; guest faults must
 * release ownership before a non-local abort. Tests use real host threads.
 * SLICE:gate hw/arm/guest-gles.c range static QemuCond gles_execution_cond; | /*\n * Guest memory behind
 * SLICE:entry hw/arm/guest-gles.c fn qc_handle_gles
 * CFLAGS -pthread
 */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdio.h>
#include <unistd.h>
typedef pthread_cond_t QemuCond;
static pthread_mutex_t bql = PTHREAD_MUTEX_INITIALIZER;
static __thread bool held;
static bool bql_locked(void)
{
    return held;
}
static void bql_lock(void)
{
    assert(!held);
    pthread_mutex_lock(&bql);
    held = true;
}
static void bql_unlock(void)
{
    assert(held);
    held = false;
    pthread_mutex_unlock(&bql);
}
static void qemu_cond_init(QemuCond *c)
{
    assert(!pthread_cond_init(c, NULL));
}
static void qemu_cond_broadcast(QemuCond *c)
{
    assert(held);
    pthread_cond_broadcast(c);
}
static void qemu_cond_wait_bql(QemuCond *c)
{
    assert(held);
    held = false;
    pthread_cond_wait(c, &bql);
    held = true;
}
#include "gate.h"
typedef struct {
    unsigned kind;
} CPUState;
typedef struct {
    unsigned unused;
} qc_gles_args_t;
static struct {
    bool armed, pending;
} gf;
static pthread_mutex_t control = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed = PTHREAD_COND_INITIALIZER;
static bool drawing, finish, waiter_started, second_entered;
static jmp_buf fault_return;
static int64_t gles_handle_call(CPUState *cpu, qc_gles_args_t *a)
{
    assert(held && gles_execution_owner);
    if (cpu->kind == 1) {
        bool unlocked = gles_native_begin();
        assert(unlocked && !held);
        pthread_mutex_lock(&control);
        drawing = true;
        pthread_cond_broadcast(&changed);
        while (!finish)
            pthread_cond_wait(&changed, &control);
        pthread_mutex_unlock(&control);
        gles_native_end(unlocked);
        assert(held);
    } else if (cpu->kind == 2) {
        assert(!second_entered);
        second_entered = true;
    } else if (cpu->kind == 3) {
        gf.pending = true;
    }
    return 73;
}
static void gles_guest_fault_in(CPUState *cpu)
{
    assert(held && !gles_execution_owner && !gles_execution_busy);
    gf.armed = false;
    if (gf.pending) {
        gf.pending = false;
        longjmp(fault_return, 1);
    }
}
#include "entry.h"
static void *render(void *unused)
{
    CPUState cpu = {1};
    qc_gles_args_t args = {0};
    bql_lock();
    assert(qc_handle_gles(&cpu, &args) == 73);
    bql_unlock();
    return NULL;
}
static void *waiter(void *unused)
{
    CPUState cpu = {2};
    qc_gles_args_t args = {0};
    bql_lock();
    pthread_mutex_lock(&control);
    waiter_started = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&control);
    assert(qc_handle_gles(&cpu, &args) == 73);
    bql_unlock();
    return NULL;
}
int main(void)
{
    alarm(10);
    assert(!gles_native_begin());
    gles_native_end(false);
    pthread_t a, b;
    assert(!pthread_create(&a, NULL, render, NULL));
    pthread_mutex_lock(&control);
    while (!drawing)
        pthread_cond_wait(&changed, &control);
    pthread_mutex_unlock(&control);
    assert(!pthread_create(&b, NULL, waiter, NULL));
    pthread_mutex_lock(&control);
    while (!waiter_started)
        pthread_cond_wait(&changed, &control);
    pthread_mutex_unlock(&control);
    /* The GL waiter must release the BQL so this unrelated I/O can run. */
    bql_lock();
    assert(gles_execution_busy && !second_entered);
    bql_unlock();
    pthread_mutex_lock(&control);
    finish = true;
    pthread_cond_broadcast(&changed);
    pthread_mutex_unlock(&control);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    assert(second_entered);
    bql_lock();
    CPUState cpu = {3};
    qc_gles_args_t args = {0};
    if (!setjmp(fault_return)) {
        qc_handle_gles(&cpu, &args);
        assert(!"fault did not abort");
    }
    assert(!gles_execution_busy && held);
    cpu.kind = 0;
    assert(qc_handle_gles(&cpu, &args) == 73);
    bql_unlock();
    puts(
        "PASS: native draw permits I/O, GL wait releases BQL, renderer serialized, fault retry releases gate");
}
