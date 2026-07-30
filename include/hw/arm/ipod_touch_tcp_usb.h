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
};

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
