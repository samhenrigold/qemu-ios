/*
 * Guest-package delivery, shared by the iPod Touch 2G and iPad 1 machines.
 * The guest half is contrib/it-boot/it_boot.c, which pulls an offer the app
 * composed into the guest-package=DIR directory, stages it and reports back
 * (docs/guest-package-bootstrap.md in the LightTouchMac tree).
 *
 * Ops, all with qc_ag_args_t and windows of at most GUEST_PKG_WINDOW bytes:
 *   QC_PKG_OFFER  offset/length window of DIR/offer; length 0 snapshots the
 *                 file and returns its size (-1: no directory or no offer)
 *   QC_PKG_READ   token = payload index from the snapshot's file/job/hook
 *                 lines; length 0 returns its size, else copies a window
 *   QC_PKG_REPORT token = serial, offset = it_boot's result code, buffer =
 *                 an optional status line
 *   QC_GLES_HELLO offset = the shim's GL wire protocol, token = its package
 *                 serial; retval = the host's protocol. A shim that never
 *                 sends it speaks protocol 0, today's wire.
 * The guest never names a path: the host maps indices to the paths in its own
 * offer, all relative to DIR.
 */
#ifndef HW_ARM_GUEST_PACKAGE_H
#define HW_ARM_GUEST_PACKAGE_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef GUEST_PKG_CORE_ONLY
#define QC_GLES_HELLO 0x142        /* the qemu_call_number_t values in general.h */
#define QC_PKG_OFFER  0x170
#define QC_PKG_READ   0x171
#define QC_PKG_REPORT 0x172
#else
#include "qom/object.h"
#include "cpu.h"
#include "hw/arm/guest-services/general.h"
#endif

#define GUEST_PKG_WINDOW    1024
#define GUEST_PKG_OFFER_MAX (64 * 1024)
#define GUEST_PKG_FILE_MAX  (16 * 1024 * 1024)
#define GUEST_PKG_ENTRIES   64
/* GL wire protocol the host serves now; it also keeps serving the one before. */
#define GUEST_GLES_PROTO    0

typedef struct GuestPackage {
    char *dir;                /* guest-package= property, NULL = no offer */
    char *offer;              /* snapshot served to the guest */
    size_t offer_len;
    char *paths[GUEST_PKG_ENTRIES];  /* payload index -> path under dir */
    char *file;               /* the payload being read, cached */
    size_t file_len;
    int file_idx;             /* -1 = none cached */
} GuestPackage;

/* Copies len bytes to (write) or from guest address; 0 on success. */
typedef int (*GuestPkgCopy)(void *opaque, uint32_t address, uint8_t *data,
                            size_t len, bool write);

/* Serve one op; returns retval (-1 on any error). */
int64_t guest_pkg_op(GuestPackage *p, unsigned op, uint64_t token,
                     uint32_t address, uint32_t offset, uint32_t length,
                     GuestPkgCopy copy, void *opaque);
void guest_pkg_set_dir(GuestPackage *p, const char *dir);
/* A guest reset: drop the snapshot and this boot's report and hello. */
void guest_pkg_reset(GuestPackage *p);

/* Readable from any thread. false/0 when nothing arrived since the reset. */
bool guest_pkg_last_report(int64_t *serial, int32_t *result);
int32_t guest_pkg_gles_protocol(int64_t *serial);
/* Owned text for the guest-package-status property. */
char *guest_pkg_status(void);

#ifndef GUEST_PKG_CORE_ONLY
/* Adds guest-package and guest-package-status to the machine object. */
void guest_pkg_init(GuestPackage *p, Object *machine);
/* Serve a QC_PKG_* or QC_GLES_HELLO request in place; false for other calls. */
bool guest_pkg_call(GuestPackage *p, CPUState *cpu, qemu_call_t *q,
                    int32_t *err);
#endif

#endif
