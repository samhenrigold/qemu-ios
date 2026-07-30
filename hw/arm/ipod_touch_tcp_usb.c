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
 * Recovered from the dfu_s5l8720 branch. Differences from the original, all of
 * them bug fixes rather than behaviour changes:
 *
 *  - I/O errors were classified by comparing the return value against
 *    EWOULDBLOCK, which is an errno value, not a return value. Every error
 *    including a genuinely dead socket therefore looked like "try again" and
 *    the state machine wedged. Classification now uses errno.
 *  - tcp_usb_cleanup() closed the fd before removing its handler, so the
 *    main loop kept polling a closed (and possibly reused) descriptor.
 *  - TCP_NODELAY is set. Every control transfer is a round trip, and Nagle
 *    turns that into a 40ms stall per packet.
 *  - The per-packet hexdumps are behind a runtime flag instead of a compile
 *    time one, so a multi-megabyte transfer does not drown the console.
 *  - Buffer ownership is explicit (owns_packet) rather than implied by which
 *    state the machine happens to be in.
 */
#include "hw/arm/ipod_touch_tcp_usb.h"

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>

static bool tcp_usb_debug(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *e = getenv("IT_USB_TCP_DEBUG");
		cached = (e && *e && *e != '0') ? 1 : 0;
	}
	return cached == 1;
}

#define debug_printf(...) \
	do { if (tcp_usb_debug()) fprintf(stderr, __VA_ARGS__); } while (0)

static void debug_hexdump(const char *_title, const void *_data, size_t _amt)
{
	if (!tcp_usb_debug() || !_data) {
		return;
	}

	const unsigned char *ptr = _data;
	fprintf(stderr, "%s", _title);
	for (size_t i = 0; i < _amt; i++) {
		if ((i % 8) == 0) {
			fprintf(stderr, "\n\t");
		}
		fprintf(stderr, "%02x ", ptr[i]);
	}
	fprintf(stderr, "\n");
}

void tcp_usb_init(tcp_usb_state_t *_state, tcp_usb_callback_t _cb, tcp_usb_closed_t _closed, void *_arg)
{
	_state->data_callback = _cb;
	_state->closed_callback = _closed;
	_state->callback_arg = _arg;

	_state->socket = -1;
	_state->closed = 1;

	_state->state = tcp_usb_idle;
	_state->amount_done = 0;
	_state->header = NULL;
	_state->buffer = NULL;
	_state->owns_packet = false;
}

void tcp_usb_cleanup(tcp_usb_state_t *_state)
{
	if (_state->socket >= 0) {
		/* Drop the handler first - the main loop must not be left polling a
		 * descriptor that is closed, or worse, recycled by a later open(). */
		qemu_set_fd_handler(_state->socket, NULL, NULL, NULL);
		close(_state->socket);
		_state->socket = -1;
	}

	if (_state->owns_packet) {
		g_free(_state->header);
		g_free(_state->buffer);
	}
	_state->header = NULL;
	_state->buffer = NULL;
	_state->owns_packet = false;
	_state->state = tcp_usb_idle;
}

int tcp_usb_closed(tcp_usb_state_t *_state)
{
	return _state->closed != 0;
}

static void tcp_usb_free_packet(tcp_usb_state_t *_state)
{
	if (!_state->owns_packet) {
		return;
	}
	g_free(_state->header);
	g_free(_state->buffer);
	_state->header = NULL;
	_state->buffer = NULL;
	_state->owns_packet = false;
}

static void tcp_usb_do_closed(tcp_usb_state_t *_state)
{
	if (_state->closed) {
		return;
	}

	_state->closed = 1;
	tcp_usb_free_packet(_state);

	if (_state->socket >= 0) {
		qemu_set_fd_handler(_state->socket, NULL, NULL, NULL);
	}

	if (_state->closed_callback) {
		_state->closed_callback(_state, _state->callback_arg);
	}
}

typedef enum {
	TCP_USB_IO_OK,      /* made progress */
	TCP_USB_IO_AGAIN,   /* would block; wait for the next fd event */
	TCP_USB_IO_CLOSED,  /* peer went away or the socket is unusable */
} tcp_usb_io_result;

/*
 * The original compared the return value against EWOULDBLOCK, so every error
 * was silently retried forever. Classify on errno instead.
 */
static tcp_usb_io_result tcp_usb_classify(ssize_t _ret, const char *_what)
{
	if (_ret > 0) {
		return TCP_USB_IO_OK;
	}
	if (_ret == 0) {
		debug_printf("tcp_usb: %s: peer closed\n", _what);
		return TCP_USB_IO_CLOSED;
	}
	if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) {
		return TCP_USB_IO_AGAIN;
	}
	fprintf(stderr, "tcp_usb: %s: %s\n", _what, strerror(errno));
	return TCP_USB_IO_CLOSED;
}

static size_t tcp_usb_payload_len(const tcp_usb_header_t *_hdr)
{
	/* length is an int16_t straight off the network and may be a negative
	 * USB_RET_* code; anything not positive means "no payload". */
	return _hdr->length > 0 ? (size_t)_hdr->length : 0;
}

static void tcp_usb_callback(tcp_usb_state_t *state, int _can_read, int _can_write)
{
	ssize_t ret;

	if (state->closed) {
		return;
	}

	switch (state->state) {
	case tcp_usb_idle:
		if (!_can_read) {
			return;
		}

		state->header = g_malloc0(sizeof(*state->header));
		state->buffer = NULL;
		state->owns_packet = true;
		state->amount_done = 0;
		state->state = tcp_usb_read_request;

		/* fall through */
	case tcp_usb_read_request:
		if (state->amount_done < sizeof(*state->header)) {
			ret = read(state->socket,
					((char *)state->header) + state->amount_done,
					sizeof(*state->header) - state->amount_done);
			switch (tcp_usb_classify(ret, "reading request header")) {
			case TCP_USB_IO_AGAIN:
				return;
			case TCP_USB_IO_CLOSED:
				tcp_usb_do_closed(state);
				return;
			default:
				break;
			}

			_can_read = 0;
			state->amount_done += ret;

			if (state->amount_done < sizeof(*state->header)) {
				return;
			}

			debug_hexdump("tcp_usb: got header:", state->header, sizeof(*state->header));

			size_t len = tcp_usb_payload_len(state->header);
			state->buffer = len > 0 ? g_malloc0(len) : NULL;
		}

		if ((state->header->ep & USB_DIR_IN) == 0 && tcp_usb_payload_len(state->header) > 0) {
			size_t len = tcp_usb_payload_len(state->header);
			size_t got = state->amount_done - sizeof(*state->header);

			ret = read(state->socket, state->buffer + got, len - got);
			switch (tcp_usb_classify(ret, "reading request data")) {
			case TCP_USB_IO_AGAIN:
				return;
			case TCP_USB_IO_CLOSED:
				tcp_usb_do_closed(state);
				return;
			default:
				break;
			}

			_can_read = 0;
			state->amount_done += ret;

			if (state->amount_done < sizeof(*state->header) + len) {
				return;
			}

			debug_hexdump("tcp_usb: read:", state->buffer, len);
		}

		if (!state->data_callback) {
			fprintf(stderr, "tcp_usb: packet received but no callback\n");
			tcp_usb_free_packet(state);
			state->state = tcp_usb_idle;
			return;
		}

		state->state = tcp_usb_write_response;
		state->amount_done = 0;
		state->header->length = state->data_callback(state, state->callback_arg,
													 state->header, state->buffer);

		/* fall through */
	case tcp_usb_write_response:
		if (state->amount_done < sizeof(*state->header)) {
			ret = write(state->socket,
					((char *)state->header) + state->amount_done,
					sizeof(*state->header) - state->amount_done);
			switch (tcp_usb_classify(ret, "writing response header")) {
			case TCP_USB_IO_AGAIN:
				return;
			case TCP_USB_IO_CLOSED:
				tcp_usb_do_closed(state);
				return;
			default:
				break;
			}

			_can_write = 0;
			state->amount_done += ret;

			if (state->amount_done < sizeof(*state->header)) {
				return;
			}

			debug_hexdump("tcp_usb: sent response header:", state->header,
						  sizeof(*state->header));
		}

		if ((state->header->ep & USB_DIR_IN) != 0 && tcp_usb_payload_len(state->header) > 0) {
			size_t len = tcp_usb_payload_len(state->header);
			size_t sent = state->amount_done - sizeof(*state->header);

			ret = write(state->socket, state->buffer + sent, len - sent);
			switch (tcp_usb_classify(ret, "writing response data")) {
			case TCP_USB_IO_AGAIN:
				return;
			case TCP_USB_IO_CLOSED:
				tcp_usb_do_closed(state);
				return;
			default:
				break;
			}

			_can_write = 0;
			state->amount_done += ret;

			if (state->amount_done < sizeof(*state->header) + len) {
				return;
			}

			debug_hexdump("tcp_usb: wrote:", state->buffer, len);
		}

		tcp_usb_free_packet(state);
		state->state = tcp_usb_idle;
		break;

	case tcp_usb_write_request:
		if (state->amount_done < sizeof(*state->header)) {
			ret = write(state->socket,
					((char *)state->header) + state->amount_done,
					sizeof(*state->header) - state->amount_done);
			switch (tcp_usb_classify(ret, "writing request header")) {
			case TCP_USB_IO_AGAIN:
				return;
			case TCP_USB_IO_CLOSED:
				tcp_usb_do_closed(state);
				return;
			default:
				break;
			}

			_can_write = 0;
			state->amount_done += ret;

			if (state->amount_done < sizeof(*state->header)) {
				return;
			}
		}

		if ((state->header->ep & USB_DIR_IN) == 0 && tcp_usb_payload_len(state->header) > 0) {
			size_t len = tcp_usb_payload_len(state->header);
			size_t sent = state->amount_done - sizeof(*state->header);

			ret = write(state->socket, state->buffer + sent, len - sent);
			switch (tcp_usb_classify(ret, "writing request data")) {
			case TCP_USB_IO_AGAIN:
				return;
			case TCP_USB_IO_CLOSED:
				tcp_usb_do_closed(state);
				return;
			default:
				break;
			}

			_can_write = 0;
			state->amount_done += ret;

			if (state->amount_done < sizeof(*state->header) + len) {
				return;
			}
		}

		state->state = tcp_usb_read_response;
		state->amount_done = 0;

		/* fall through */
	case tcp_usb_read_response:
		if (state->amount_done < sizeof(*state->header)) {
			ret = read(state->socket,
					((char *)state->header) + state->amount_done,
					sizeof(*state->header) - state->amount_done);
			switch (tcp_usb_classify(ret, "reading response header")) {
			case TCP_USB_IO_AGAIN:
				return;
			case TCP_USB_IO_CLOSED:
				tcp_usb_do_closed(state);
				return;
			default:
				break;
			}

			_can_read = 0;
			state->amount_done += ret;

			if (state->amount_done < sizeof(*state->header)) {
				return;
			}
		}

		if ((state->header->ep & USB_DIR_IN) != 0 && tcp_usb_payload_len(state->header) > 0) {
			size_t len = tcp_usb_payload_len(state->header);
			size_t got = state->amount_done - sizeof(*state->header);

			ret = read(state->socket, state->buffer + got, len - got);
			switch (tcp_usb_classify(ret, "reading response data")) {
			case TCP_USB_IO_AGAIN:
				return;
			case TCP_USB_IO_CLOSED:
				tcp_usb_do_closed(state);
				return;
			default:
				break;
			}

			_can_read = 0;
			state->amount_done += ret;

			if (state->amount_done < sizeof(*state->header) + len) {
				return;
			}
		}

		state->state = tcp_usb_idle;

		if (state->data_callback) {
			state->data_callback(state, state->callback_arg, state->header, state->buffer);
		} else {
			fprintf(stderr, "tcp_usb: request sent but no callback\n");
		}
		return;
	}
}

static void tcp_usb_read_callback(void *_arg)
{
	tcp_usb_callback((tcp_usb_state_t *)_arg, 1, 0);
}

static void tcp_usb_write_callback(void *_arg)
{
	tcp_usb_callback((tcp_usb_state_t *)_arg, 0, 1);
}

int tcp_usb_connect(tcp_usb_state_t *_state, const char *_host, uint32_t _port)
{
	struct addrinfo hints = {
		.ai_family = AF_INET,
		.ai_socktype = SOCK_STREAM,
	};
	struct addrinfo *res = NULL;
	char port[16];
	int fd;

	snprintf(port, sizeof(port), "%u", _port);
	if (getaddrinfo(_host, port, &hints, &res) != 0 || !res) {
		return -EIO;
	}

	fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) {
		freeaddrinfo(res);
		return -EIO;
	}

	if (connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
		close(fd);
		freeaddrinfo(res);
		return -EIO;
	}
	freeaddrinfo(res);

	/* Every control transfer is a round trip; Nagle would add 40ms to each. */
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	_state->socket = fd;
	_state->closed = 0;
	_state->state = tcp_usb_idle;
	_state->amount_done = 0;

	int flags = fcntl(fd, F_GETFL, 0);
	fcntl(fd, F_SETFL, flags | O_NONBLOCK);
	qemu_set_fd_handler(fd, tcp_usb_read_callback, tcp_usb_write_callback, _state);
	return 0;
}

int tcp_usb_request(tcp_usb_state_t *_state, tcp_usb_header_t *_header, const char *_data)
{
	if (_state->closed) {
		return -ENOTCONN;
	}
	if (_state->state != tcp_usb_idle) {
		return -EBUSY;
	}

	_state->state = tcp_usb_write_request;
	_state->amount_done = 0;
	_state->buffer = (char *)_data;
	_state->header = _header;
	_state->owns_packet = false; /* caller owns these */

	tcp_usb_callback(_state, 0, 1);
	return 0;
}
