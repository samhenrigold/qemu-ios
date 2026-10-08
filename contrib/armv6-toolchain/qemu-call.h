/*
 * The guest side of the cp15 QEMU_CALL trap, for every guest tool. The host
 * side is include/hw/arm/guest-services/general.h, whose note explains why the
 * 52-byte layout is frozen: call_number(4) + args(32) + retval(8) + error(8).
 *
 * Freestanding (no headers, no stdint) so the -nostdinc legacy.h builds can
 * include it; r9 stays untouched.
 */
#ifndef QEMU_CALL_GUEST_H
#define QEMU_CALL_GUEST_H

#define QC_GLES_INLINE_ARGS 4

typedef struct __attribute__((packed)) {
    unsigned int call_number;
    union {
        unsigned char raw[32];
        struct __attribute__((packed)) {        /* qc_ag_args_t (and qc_pb_args_t) */
            unsigned int buffer;
            unsigned int offset;
            unsigned int length;
            unsigned long long token;
        } ag;
        struct __attribute__((packed)) {        /* qc_gles_args_t */
            unsigned int slot;
            unsigned int ctx;
            unsigned int argc;
            unsigned int spill;
            unsigned int args[QC_GLES_INLINE_ARGS];
        } gles;
    };
    long long retval;
    long long error;
} qemu_call_t;

typedef char qemu_call_t_is_52_bytes[sizeof(qemu_call_t) == 52 ? 1 : -1];

/* The host reads *q, runs the call, and writes retval and error back. */
static inline void qemu_call_trap(volatile qemu_call_t *q)
{
    __asm__ volatile("mcr p15, 3, %0, c15, c15, 0" : : "r"(q) : "memory");
}

#endif
