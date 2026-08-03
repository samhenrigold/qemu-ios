/*
 * QEMU TCP Tunnelling
 *
 * Copyright (c) 2019 Lev Aronsky <aronsky@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#ifndef HW_ARM_GUEST_SERVICES_GENERAL_H
#define HW_ARM_GUEST_SERVICES_GENERAL_H

#include "hw/arm/guest-services/socket.h"
#include "hw/arm/guest-services/fds.h"
#include "hw/arm/guest-services/file.h"
#include "hw/arm/guest-services/gles.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wredundant-decls"
extern int32_t guest_svcs_errno;
#pragma GCC diagnostic pop

typedef enum {
    // File Descriptors API
    QC_CLOSE = 0x100,
    QC_FCNTL,

    // Socket API
    QC_SOCKET = 0x110,
    QC_ACCEPT,
    QC_BIND,
    QC_CONNECT,
    QC_LISTEN,
    QC_RECV,
    QC_SEND,
    QC_WRITE_FILE,
    QC_READ_FILE,
    QC_SIZE_FILE,

    // Host keyboard -> guest text input. QC_POLL_INPUT dequeues one unichar
    // from the machine's host-keyboard ring (see ipod_touch_kbd_event) into
    // retval, or returns 0 when the ring is empty. A small injected guest
    // agent polls this and feeds each char to _GSPostSyntheticKeyEvent.
    QC_POLL_INPUT = 0x130,

    // OpenGL ES 1.1 high-level emulation. One request per GL entry point, from
    // the guest-side MBXGLEngine replacement. See guest-services/gles.h.
    //
    // Unlike every other call here, this one is issued from PL0: the QEMU_CALL
    // cp15 register is declared PL0_RW and cp_access_ok honours that, so an
    // unprivileged `mcr p15,3,r0,c15,c15,0` traps straight to the host with no
    // kernel patch in the way. That is what makes a pure userspace shim
    // possible -- measured, not assumed; see the QC_GLES_PING probe.
    QC_GLES = 0x140,

    // Liveness probe for the PL0 trap path. Returns QC_GLES_PING_MAGIC so a
    // guest test can tell "the host answered" apart from "the mcr was a nop",
    // which is otherwise indistinguishable: an unhandled cp15 write on this
    // machine silently does nothing rather than faulting.
    QC_GLES_PING = 0x141,
} qemu_call_number_t;

#define QC_GLES_PING_MAGIC 0x6a17c0deLL

typedef struct __attribute__((packed)) {
    // Request
    qemu_call_number_t call_number;
    union {
        // File Descriptors API
        qc_close_args_t close;
        qc_fcntl_args_t fcntl;
        // Socket API
        qc_socket_args_t socket;
        qc_accept_args_t accept;
        qc_bind_args_t bind;
        qc_connect_args_t connect;
        qc_listen_args_t listen;
        qc_recv_args_t recv;
        qc_send_args_t send;
        qc_write_file_args_t write_file;
        qc_read_file_args_t read_file;
        qc_size_file_args_t size_file;
        // OpenGL ES HLE
        qc_gles_args_t gles;
    } args;

    // Response
    int64_t retval;
    int64_t error;
} qemu_call_t;

// The guest agents that use this protocol are compiled separately and shipped
// *inside NAND images* (contrib/it-kbd-agent is already injected into images we
// cannot rebuild). They hardcode this layout as
// call_number(4) + args(32) + retval(8) + error(8) = 52.
//
// So the args union's size is frozen. Adding a request whose args struct is
// wider than 32 bytes would move retval out from under every already-deployed
// agent, and nothing at build time would say so -- the guest would just start
// reading garbage where its return value used to be. Keep new args structs at
// or under 32 bytes; if the protocol ever genuinely has to grow, every injected
// agent has to be rebuilt and every image re-injected in the same change.
#ifndef OUT_OF_TREE_BUILD
QEMU_BUILD_BUG_MSG(sizeof(qemu_call_t) != 52,
                   "qemu_call_t layout changed: guest agents already compiled "
                   "into existing NAND images expect exactly 52 bytes");
#endif

#ifndef OUT_OF_TREE_BUILD
struct ARMCPRegInfo;
uint64_t qemu_call_status(CPUARMState *env, const struct ARMCPRegInfo *ri);
void qemu_call(CPUARMState *env, const struct ARMCPRegInfo *ri, uint64_t value);
#else
uint32_t qemu_call_status(qemu_call_t *qcall);
void qemu_call(qemu_call_t *qcall);
#endif

#endif // HW_ARM_GUEST_SERVICES_GENERAL_H
