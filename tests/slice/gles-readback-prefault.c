/* Writable/COW readback pages must be touched with real stores, preserving
 * contents and respecting the requested range and its guard pages.
 * SLICE contrib/it-gles/mbxshim.c fn guest_fault_write
 * CFLAGS -Wall -Wextra
 */
#include <assert.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include "slice.h"
static unsigned char *buffer;
static size_t page_size;
static volatile sig_atomic_t faults, touched[3];
static void write_fault(int signal, siginfo_t *info, void *context)
{
    (void)signal;
    (void)context;
    uintptr_t address = (uintptr_t)info->si_addr;
    if (address < (uintptr_t)buffer ||
        address >= (uintptr_t)buffer + 3 * page_size)
        _exit(90);
    unsigned index = (address - (uintptr_t)buffer) / page_size;
    if (touched[index]++)
        _exit(91);
    if (mprotect(buffer + index * page_size, page_size, PROT_READ | PROT_WRITE))
        _exit(92);
    faults++;
}
int main(void)
{
    page_size = sysconf(_SC_PAGESIZE);
    unsigned char *mapping =
        mmap(NULL, 5 * page_size, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
    assert(mapping != MAP_FAILED);
    buffer = mapping + page_size;
    assert(!mprotect(buffer, 3 * page_size, PROT_READ | PROT_WRITE));
    memset(buffer, 0x59, 3 * page_size);
    assert(!mprotect(buffer, 3 * page_size, PROT_READ));
    struct sigaction action = {0}, old_segv, old_bus;
    action.sa_sigaction = write_fault;
    action.sa_flags = SA_SIGINFO;
    sigemptyset(&action.sa_mask);
    assert(!sigaction(SIGSEGV, &action, &old_segv));
    assert(!sigaction(SIGBUS, &action, &old_bus));
    assert(guest_fault_write((unsigned long)(buffer + 17), 3 * page_size - 37));
    assert(faults == 3 && touched[0] == 1 && touched[1] == 1 &&
           touched[2] == 1);
    for (size_t i = 0; i < 3 * page_size; i++)
        assert(buffer[i] == 0x59);
    assert(!guest_fault_write(0, 1));
    assert(!guest_fault_write((unsigned long)buffer, 0));
    assert(!guest_fault_write((unsigned long)buffer, 64u * 1024 * 1024 + 1));
    assert(!guest_fault_write(~0UL - 1, 8));
    assert(!sigaction(SIGSEGV, &old_segv, NULL));
    assert(!sigaction(SIGBUS, &old_bus, NULL));
    assert(!munmap(mapping, 5 * page_size));
    puts(
        "PASS: readback prefault resolves every write-protected page, preserves bytes and guard pages");
}
