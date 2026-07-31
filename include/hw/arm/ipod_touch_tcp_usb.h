#ifndef HW_ARM_IPOD_TOUCH_TCP_USB_H
#define HW_ARM_IPOD_TOUCH_TCP_USB_H

/*
 * USB over TCP transport.
 *
 * Copyright (c) 2011 Richard Ian Taylor.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, see <http://www.gnu.org/licenses/>.
 *
 * Recovered from the dfu_s5l8720 branch, where it was dropped when the OTG
 * model was reduced to registers only.
 *
 * Wire protocol: a 5-byte packed header followed by an optional payload. It is
 * strictly host-driven request/response - the device never initiates, so IN
 * data only flows when the host polls. A response length may be a negative
 * USB_RET_* code (NAK/STALL); the host must treat negative as "retry", which is
 * the entire flow-control mechanism. The device overwrites hdr->addr with its
 * DCFG address, which is how the host learns SET_ADDRESS took effect.
 */

#include "qemu/osdep.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/main-loop.h"
#include "hw/usb.h"
#include "hw/sysbus.h"

enum
{
	tcp_usb_setup = 1 << 0,
	tcp_usb_reset = 1 << 1,
	tcp_usb_enumdone = 1 << 2,
	tcp_usb_hello = 1 << 3,
};

/*
 * Version handshake. See docs/tcp-usb-protocol.md for the full specification.
 *
 * The host opens with an IN request on TCP_USB_EP_CONTROL carrying the hello
 * flag; the device answers with a tcp_usb_hello_t. This exists because the two
 * halves live in separate repositories and previously agreed on the transport's
 * constraints only by convention -- a disagreement about the maximum
 * transaction size silently corrupted every bulk transfer larger than the
 * guest's armed size instead of failing. The device now states its limit and the
 * host is required to honour it.
 *
 * A device that predates the handshake sees an endpoint outside its range and
 * stalls, so an old device fails loudly against a new host rather than
 * mysteriously; a new device is unaffected by a host that never asks.
 */
#define TCP_USB_EP_CONTROL         0x7f
#define TCP_USB_HELLO_MAGIC        0x42535554u  /* "TUSB" */
#define TCP_USB_PROTOCOL_VERSION   1

/*
 * The header's length is an int16_t, so a single transaction can never carry
 * more than INT16_MAX bytes. The host must also never split one logical packet
 * across transactions: this transport is transfer-oriented, and the device
 * retires the endpoint after each transaction, so a continuation would land in
 * a transfer the guest already considers finished.
 */
#define TCP_USB_MAX_TRANSACTION    32767

typedef struct _tcp_usb_hello
{
	uint32_t magic;            /* TCP_USB_HELLO_MAGIC */
	uint16_t version;          /* TCP_USB_PROTOCOL_VERSION */
	uint16_t reserved;
	uint32_t max_transaction;  /* largest payload the device will accept whole */

} __attribute__((packed)) tcp_usb_hello_t;

typedef enum _tcp_usb_state_enum
{
	tcp_usb_idle,

	/* Client (the emulated device) */
	tcp_usb_read_request,
	tcp_usb_write_response,

	/* Host */
	tcp_usb_write_request,
	tcp_usb_read_response,

} tcp_usb_state_enum_t;

typedef struct _tcp_usb_header
{
	uint8_t addr;
	uint8_t ep;
	uint8_t flags;
	int16_t length;

} __attribute__((packed)) tcp_usb_header_t;

struct _tcp_usb_state;
typedef int (*tcp_usb_callback_t)(struct _tcp_usb_state *_status, void *_arg, tcp_usb_header_t *_hdr, char *_buffer);
typedef void (*tcp_usb_closed_t)(struct _tcp_usb_state *_state, void *_arg);

typedef struct _tcp_usb_state
{
	int socket;
	int closed;

	tcp_usb_state_enum_t state;
	tcp_usb_header_t *header;
	char *buffer;
	size_t amount_done;

	/* True while header/buffer are owned by us and must be freed. */
	bool owns_packet;

	tcp_usb_closed_t closed_callback;
	tcp_usb_callback_t data_callback;
	void *callback_arg;
} tcp_usb_state_t;

void tcp_usb_init(tcp_usb_state_t *_state, tcp_usb_callback_t _cb, tcp_usb_closed_t _closed, void *_arg);
void tcp_usb_cleanup(tcp_usb_state_t *_state);

int tcp_usb_closed(tcp_usb_state_t *_state);

int tcp_usb_connect(tcp_usb_state_t *_state, const char *_host, uint32_t _port);

int tcp_usb_request(tcp_usb_state_t *_state, tcp_usb_header_t *_header, const char *_data);

#endif /* HW_ARM_IPOD_TOUCH_TCP_USB_H */
