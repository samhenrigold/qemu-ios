/*
 * Synopsys DesignWareCore for USB OTG.
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
 */
#include "qemu/osdep.h"
#include "hw/platform-bus.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "hw/hw.h"
#include "hw/arm/ipod_touch_usb_otg.h"
#include "migration/vmstate.h"

/*
 * Phase 0 diagnostics. This model was written against openiBoot, so the iOS
 * AppleSynopsysOTGDevice driver is expected to touch registers it does not
 * implement. Every such access used to hw_error() and abort QEMU, which made
 * it impossible to observe what the driver actually wants; they now log via
 * LOG_UNIMP (visible with -d unimp) and return 0.
 *
 * Set IT_USB_TRACE=1 in the environment for a full read/write trace.
 */
static bool synopsys_usb_trace_enabled(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *e = getenv("IT_USB_TRACE");
		cached = (e && *e && *e != '0') ? 1 : 0;
	}
	return cached == 1;
}

/*
 * Separate, finer-grained switch for the device->host (IN) path: every endpoint
 * arm the guest performs and every transaction the host pulls, with the first
 * bytes of the data actually read out of guest memory. IT_USB_TRACE is too
 * coarse (and too noisy) to answer where a bulk IN transfer's bytes come from.
 */
static bool synopsys_usb_in_debug(void)
{
	static int cached = -1;
	if (cached < 0) {
		const char *e = getenv("IT_USB_IN_DEBUG");
		cached = (e && *e && *e != '0') ? 1 : 0;
	}
	return cached == 1;
}

static void synopsys_usb_update_irq(synopsys_usb_state *_state)
{
	_state->daintsts = 0;
	_state->gintsts &=~ (GINTMSK_OEP | GINTMSK_INEP | GINTMSK_OTG);

	if(_state->gotgint)
		_state->gintsts |= GINTMSK_OTG;

	int i;
	for(i = 0; i < USB_NUM_ENDPOINTS; i++)
	{
		if(_state->out_eps[i].interrupt_status & _state->doepmsk)
		{
			_state->daintsts |= 1 << (i+DAINT_OUT_SHIFT);
			if(_state->daintmsk & (1 << (i+DAINT_OUT_SHIFT)))
				_state->gintsts |= GINTMSK_OEP;
		}

		if(_state->in_eps[i].interrupt_status & _state->diepmsk)
		{
			_state->daintsts |= 1 << (i+DAINT_IN_SHIFT);
			if(_state->daintmsk & (1 << (i+DAINT_IN_SHIFT)))
				_state->gintsts |= GINTMSK_INEP;
		}
	}
	
	if((_state->pcgcctl & 3) == 0 && _state->gintmsk & _state->gintsts)
	{
		//printf("USB: IRQ triggered 0x%08x & 0x%08x.\n", _state->gintsts, _state->gintmsk);
		qemu_irq_raise(_state->irq);
	}
	else
		qemu_irq_lower(_state->irq);
}

static void synopsys_usb_update_ep(synopsys_usb_state *_state, synopsys_usb_ep_state *_ep)
{
	if(_ep->control & USB_EPCON_SETNAK)
	{
		_ep->control |= USB_EPCON_NAKSTS;
		_ep->interrupt_status |= USB_EPINT_INEPNakEff;
		_ep->control &=~ USB_EPCON_SETNAK;
	}

	if(_ep->control & USB_EPCON_DISABLE)
	{
		_ep->interrupt_status |= USB_EPINT_EPDisbld;
		_ep->control &=~ (USB_EPCON_DISABLE | USB_EPCON_ENABLE);
	}
}

static void synopsys_usb_update_in_ep(synopsys_usb_state *_state, uint8_t _ep)
{
	synopsys_usb_ep_state *eps = &_state->in_eps[_ep];
	synopsys_usb_update_ep(_state, eps);

	if(eps->control & USB_EPCON_ENABLE)
		;//printf("USB: IN transfer queued on %d.\n", _ep);
}

static void synopsys_usb_update_out_ep(synopsys_usb_state *_state, uint8_t _ep)
{
	synopsys_usb_ep_state *eps = &_state->out_eps[_ep];
	synopsys_usb_update_ep(_state, eps);

	if(eps->control & USB_EPCON_ENABLE)
		;//printf("USB: OUT transfer queued on %d.\n", _ep);
}

/*
 * Host transport callback. Recovered from the dfu_s5l8720 branch.
 *
 * Every hw_error() in the original has been removed: all of them were reachable
 * from host-supplied data (a length off the network, a FIFO index the guest
 * chose), so a malformed or hostile packet could abort QEMU. They are clamps
 * and logged rejections now.
 *
 * This path is DMA-only, which is correct for iOS: AppleSynopsysOTG2 programs
 * GAHBCFG with DMAEN set and never touches GRXSTSP.
 */
static int synopsys_usb_tcp_callback(tcp_usb_state_t *_state, void *_arg,
                                     tcp_usb_header_t *_hdr, char *_buffer)
{
	synopsys_usb_state *state = _arg;

	/* Tell the host what address we answer to - this is how it learns that a
	 * SET_ADDRESS actually took effect. */
	_hdr->addr = (state->dcfg & DCFG_DEVICEADDRMSK) >> DCFG_DEVICEADDR_SHIFT;

	if (_hdr->flags & tcp_usb_reset) {
		state->gintsts |= GINTMSK_RESET;
		synopsys_usb_update_irq(state);
		return 0;
	}

	if (_hdr->flags & tcp_usb_enumdone) {
		state->gintsts |= GINTMSK_ENUMDONE;
	}

	/* length arrives as an int16_t; negative means "no payload", never a size. */
	size_t hdr_len = _hdr->length > 0 ? (size_t)_hdr->length : 0;
	uint8_t ep = _hdr->ep & 0x7f;
	int ret;

	/*
	 * Version handshake, answered before the endpoint range check so that a
	 * device predating it still stalls (which is how a new host detects an old
	 * one) rather than being mistaken for a working peer.
	 */
	if ((_hdr->flags & tcp_usb_hello) && ep == TCP_USB_EP_CONTROL) {
		tcp_usb_hello_t hello = {
			.magic = TCP_USB_HELLO_MAGIC,
			.version = TCP_USB_PROTOCOL_VERSION,
			.reserved = 0,
			.max_transaction = TCP_USB_MAX_TRANSACTION,
		};

		if (!(_hdr->ep & USB_DIR_IN) || hdr_len < sizeof(hello)) {
			qemu_log_mask(LOG_GUEST_ERROR,
			              "usb_synopsys: malformed hello (ep 0x%02x, %zu bytes)\n",
			              _hdr->ep, hdr_len);
			return USB_RET_STALL;
		}

		memcpy(_buffer, &hello, sizeof(hello));
		printf("[USBTCP] handshake: protocol v%u, max transaction %u bytes\n",
		       TCP_USB_PROTOCOL_VERSION, TCP_USB_MAX_TRANSACTION);
		return sizeof(hello);
	}

	if (ep >= USB_NUM_ENDPOINTS) {
		qemu_log_mask(LOG_GUEST_ERROR,
		              "usb_synopsys: host addressed out-of-range EP %d\n", ep);
		synopsys_usb_update_irq(state);
		return USB_RET_STALL;
	}

	if (_hdr->ep & USB_DIR_IN) {
		synopsys_usb_ep_state *eps = &state->in_eps[ep];

		if (eps->control & USB_EPCON_STALL) {
			eps->control &= ~USB_EPCON_STALL;
			ret = USB_RET_STALL;
		} else if (eps->control & USB_EPCON_ENABLE) {
			size_t sz = eps->tx_size & DEPTSIZ_XFERSIZ_MASK;
			size_t amtDone = MIN(sz, hdr_len);
			uint32_t dma_before = eps->dma_address;
			bool no_dma = false;

			/*
			 * DMA mode: the data path is guest memory, not the FIFO RAM, so read
			 * straight into the caller's buffer. Staging through state->fifos and
			 * clamping to the TX FIFO depth (a WORD count, misused as a byte
			 * limit) is what truncated large transfers. The genuine bounds are
			 * the guest's armed transfer size and the host's buffer, both already
			 * applied via MIN(sz, hdr_len).
			 */
			if (amtDone > 0 && eps->dma_address) {
				cpu_physical_memory_read(eps->dma_address, _buffer, amtDone);
				eps->dma_address += amtDone;
			} else if (amtDone > 0) {
				/* No DMA address programmed - nothing meaningful to send. */
				amtDone = 0;
				no_dma = true;
			}

			/*
			 * An armed IN transfer is frequently larger than one host
			 * transaction: the guest hands the pipe a whole mux packet, and
			 * AppleSynopsysOTG2 arms it as one contiguous physical segment - 32764
			 * bytes has been observed, against a host that asks for 16384 at a
			 * time. This used to disable the endpoint and raise XferCompl on the
			 * first transaction regardless, so the guest was told the whole
			 * transfer had gone out while the residue was thrown away. That is
			 * what corrupted every large device->host read: the host received a
			 * mux header declaring 32764 bytes followed by only 16384, then took
			 * the *next* packet's bytes as the continuation.
			 *
			 * Real hardware retires an IN transfer when XferSize drains, not when
			 * one host transaction ends, so keep the endpoint armed and stay
			 * silent until it does. The host then sees exactly-MRU chunks
			 * followed by a short final one, which is ordinary USB framing and
			 * what its reassembly already expects.
			 */
			size_t remaining = sz - amtDone;

			eps->tx_size = (eps->tx_size & ~DEPTSIZ_XFERSIZ_MASK)
			             | (remaining & DEPTSIZ_XFERSIZ_MASK);

			/*
			 * PktCnt counts down alongside XferSize on real hardware. Nothing in
			 * the guest driver has been observed to read it back mid-transfer,
			 * but leaving it stale would be a trap for whatever does.
			 *
			 * Only for the bulk endpoints: on EP0 the MPS field is a two-bit
			 * enum (0 = 64 bytes ... 3 = 8) rather than a byte count, and EP0
			 * transfers are single-transaction anyway.
			 */
			uint32_t mps = eps->control & USB_EPCON_MPS_MASK;
			if (ep && mps && amtDone) {
				uint32_t pktcnt = (eps->tx_size >> DEPTSIZ_PKTCNT_SHIFT)
				                & DEPTSIZ_PKTCNT_MASK;
				uint32_t sent = (amtDone + mps - 1) / mps;

				pktcnt = sent < pktcnt ? pktcnt - sent : 0;
				eps->tx_size = (eps->tx_size
				                & ~(DEPTSIZ_PKTCNT_MASK << DEPTSIZ_PKTCNT_SHIFT))
				             | (pktcnt << DEPTSIZ_PKTCNT_SHIFT);
			}

			/*
			 * A transfer the guest armed with no DMA address can never drain, so
			 * retire it rather than wedging the endpoint armed forever.
			 */
			if (remaining == 0 || no_dma) {
				eps->control &= ~USB_EPCON_ENABLE;
				eps->interrupt_status |= USB_EPINT_XferCompl;
			}

			/*
			 * Gated: this fires once per transaction, so under a multi-megabyte
			 * AFC transfer it would be tens of thousands of synchronous stdio
			 * writes on the data path. Everything else here is behind the same
			 * flag; keep this consistent so a bulk transfer runs quietly.
			 */
			if (synopsys_usb_trace_enabled())
				fprintf(stderr, "[USBTCP] IN  ep%d %zu bytes\n", ep, amtDone);
			if (synopsys_usb_in_debug()) {
				static unsigned long in_seq;
				uint8_t head[16] = {0};
				const uint8_t *b = head;
				memcpy(head, _buffer, MIN(amtDone, sizeof(head)));
				fprintf(stderr,
				        "[USBIN] #%lu ep%d dma=0x%08x armed=%zu req=%zu got=%zu left=%zu "
				        "head=%02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x %02x%02x%02x%02x\n",
				        ++in_seq, ep, dma_before, sz, hdr_len, amtDone, remaining,
				        b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7],
				        b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
			}
			ret = amtDone;
		} else {
			if (synopsys_usb_trace_enabled())
				fprintf(stderr, "[USBTCP] NAK IN  ep%d ctl=0x%08x tsiz=0x%08x "
				        "gintsts=0x%08x gintmsk=0x%08x dcfg=0x%08x\n",
				        ep, eps->control, eps->tx_size,
				        state->gintsts, state->gintmsk, state->dcfg);
			ret = USB_RET_NAK;
		}
	} else {
		synopsys_usb_ep_state *eps = &state->out_eps[ep];

		if (eps->control & USB_EPCON_STALL) {
			eps->control &= ~USB_EPCON_STALL;
			ret = USB_RET_STALL;
		} else if (eps->control & USB_EPCON_ENABLE) {
			eps->control &= ~USB_EPCON_ENABLE;

			size_t sz = eps->tx_size & DEPTSIZ_XFERSIZ_MASK;
			size_t amtDone = MIN(sz, hdr_len);

			/*
			 * DMA mode: write straight to guest memory.
			 *
			 * The original clamped this byte count against GRXFSIZ, which is the
			 * receive FIFO depth in 32-bit WORDS and is not even the data path in
			 * DMA mode. With the value iOS programs (0x11b = 283) every bulk OUT
			 * over 283 bytes was truncated - a 316-byte mux packet became 283 + 33
			 * - and since the endpoint is disabled after each transaction the
			 * guest saw two malformed packets instead of one. That is what made
			 * lockdownd time out and RST. The real bounds are the guest's armed
			 * transfer size and the host's payload, already applied above.
			 */

			uint32_t dma_before = eps->dma_address;
			bool wrote = amtDone > 0 && _buffer && eps->dma_address;

			if (wrote) {
				cpu_physical_memory_write(eps->dma_address, _buffer, amtDone);
				eps->dma_address += amtDone;
			}

			if (synopsys_usb_trace_enabled()) {
				/*
				 * Read the bytes straight back from the address we just wrote.
				 * If they do not match what we sent, the write is not landing in
				 * the memory the guest is reading -- which is the open question
				 * behind the AFC content corruption.
				 */
				uint8_t sent[8] = {0}, back[8] = {0};
				size_t n = MIN(amtDone, sizeof(sent));
				if (wrote) {
					memcpy(sent, _buffer, n);
					cpu_physical_memory_read(dma_before, back, n);
				}
				fprintf(stderr,
				        "[USBDMA] OUT ep%d hdr_len=%zu armed=%zu got=%zu dma 0x%08x"
				        "%s sent=%02x%02x%02x%02x%02x%02x%02x%02x "
				        "back=%02x%02x%02x%02x%02x%02x%02x%02x %s\n",
				        ep, hdr_len, sz, amtDone, dma_before,
				        wrote ? "" : " [NO WRITE - dma is 0]",
				        sent[0], sent[1], sent[2], sent[3],
				        sent[4], sent[5], sent[6], sent[7],
				        back[0], back[1], back[2], back[3],
				        back[4], back[5], back[6], back[7],
				        (wrote && memcmp(sent, back, n) == 0) ? "MATCH" : "MISMATCH");
			}

			/*
			 * One host transaction is one complete transfer, so it always
			 * retires the endpoint. That is right for this transport - it is
			 * transfer-oriented, not packet-oriented, and there are no ZLPs to
			 * terminate a transfer that happens to be a multiple of the max
			 * packet size.
			 *
			 * The corollary is a real constraint on the host: it must never
			 * split one logical packet across transactions, because the second
			 * half would land in a transfer the guest already considers
			 * finished. The wire header's length is an int16_t, so a host
			 * transaction cannot exceed 32767 bytes and the host must keep its
			 * MTU at or under that. usbmuxd's default USB_MTU of 49152 does not
			 * fit and silently corrupted AFC writes until it was capped.
			 *
			 * Warn when a transaction is truncated, since that is exactly the
			 * situation that produces the corruption and it is otherwise silent.
			 */
			if (amtDone < hdr_len) {
				qemu_log_mask(LOG_GUEST_ERROR,
				              "usb_synopsys: OUT ep%d truncated %zu -> %zu bytes; "
				              "the host must not split a logical packet\n",
				              ep, hdr_len, amtDone);
			}

			if (_hdr->flags & tcp_usb_setup) {
				eps->interrupt_status |= USB_EPINT_SetUp;
			} else {
				eps->interrupt_status |= USB_EPINT_XferCompl;
			}

			eps->tx_size = (eps->tx_size & ~DEPTSIZ_XFERSIZ_MASK)
			             | ((sz - amtDone) & DEPTSIZ_XFERSIZ_MASK);

			/* Gated for the same reason as the IN path above. */
			if (synopsys_usb_trace_enabled())
				fprintf(stderr, "[USBTCP] OUT ep%d %zu bytes%s\n", ep, amtDone,
				        (_hdr->flags & tcp_usb_setup) ? " (SETUP)" : "");
			ret = amtDone;
		} else {
			if (synopsys_usb_trace_enabled())
				fprintf(stderr, "[USBTCP] NAK OUT ep%d%s ctl=0x%08x tsiz=0x%08x "
				        "gintsts=0x%08x gintmsk=0x%08x dcfg=0x%08x pcgcctl=0x%08x\n",
				        ep, (_hdr->flags & tcp_usb_setup) ? " (SETUP)" : "",
				        eps->control, eps->tx_size,
				        state->gintsts, state->gintmsk, state->dcfg, state->pcgcctl);
			ret = USB_RET_NAK;
		}
	}

	synopsys_usb_update_irq(state);
	return ret;
}

/*
 * Bring up the host link if it is configured and not already up. Safe to call
 * repeatedly - a missing host bridge is not fatal, it just means no cable.
 *
 * Configured with IT_USB_TCP=host:port (default port 1235). QEMU is the client
 * and dials out, matching the original protocol direction.
 */
static void synopsys_usb_tcp_start(synopsys_usb_state *_state)
{
	if (_state->tcp_connected && !tcp_usb_closed(&_state->tcp_state)) {
		return;
	}

	if (!_state->server_host) {
		const char *spec = getenv("IT_USB_TCP");
		if (!spec || !*spec) {
			return;
		}

		char *dup = g_strdup(spec);
		char *colon = strrchr(dup, ':');
		if (colon) {
			*colon = '\0';
			_state->server_port = atoi(colon + 1);
		}
		if (!_state->server_port) {
			_state->server_port = 1235;
		}
		_state->server_host = g_strdup(*dup ? dup : "127.0.0.1");
		g_free(dup);
	}

	if (_state->tcp_connected) {
		tcp_usb_cleanup(&_state->tcp_state);
		_state->tcp_connected = false;
	}

	tcp_usb_init(&_state->tcp_state, synopsys_usb_tcp_callback, NULL, _state);

	int ret = tcp_usb_connect(&_state->tcp_state, _state->server_host,
	                          _state->server_port);
	if (ret < 0) {
		printf("[USBTCP] no host bridge at %s:%u (%d) - will retry on core reset\n",
		       _state->server_host, _state->server_port, ret);
		return;
	}

	_state->tcp_connected = true;
	printf("[USBTCP] connected to host bridge at %s:%u\n",
	       _state->server_host, _state->server_port);
}

static uint32_t synopsys_usb_in_ep_read(synopsys_usb_state *_state, uint8_t _ep, hwaddr _addr)
{
	if(_ep >= USB_NUM_ENDPOINTS)
	{
		qemu_log_mask(LOG_UNIMP, "usb_synopsys: read from out-of-range IN EP %d\n", _ep);
		return 0;
	}

    switch (_addr)
	{
    case 0x00:
        return _state->in_eps[_ep].control;

    case 0x08:
        return _state->in_eps[_ep].interrupt_status;

    case 0x10:
        return _state->in_eps[_ep].tx_size;

    case 0x14:
        return _state->in_eps[_ep].dma_address;

    case 0x1C:
        return _state->in_eps[_ep].dma_buffer;

    default:
        qemu_log_mask(LOG_UNIMP, "usb_synopsys: unimplemented IN EP%d read offset 0x"
                      HWADDR_FMT_plx "\n", _ep, _addr);
		break;
    }

	return 0;
}

static uint32_t synopsys_usb_out_ep_read(synopsys_usb_state *_state, int _ep, hwaddr _addr)
{
	if(_ep >= USB_NUM_ENDPOINTS)
	{
		qemu_log_mask(LOG_UNIMP, "usb_synopsys: read from out-of-range OUT EP %d\n", _ep);
		return 0;
	}

    switch (_addr)
	{
    case 0x00:
        return _state->out_eps[_ep].control;

    case 0x08:
        return _state->out_eps[_ep].interrupt_status;

    case 0x10:
        return _state->out_eps[_ep].tx_size;

    case 0x14:
        return _state->out_eps[_ep].dma_address;

    case 0x1C:
        return _state->out_eps[_ep].dma_buffer;

    default:
        qemu_log_mask(LOG_UNIMP, "usb_synopsys: unimplemented OUT EP%d read offset 0x"
                      HWADDR_FMT_plx "\n", _ep, _addr);
		break;
    }

	return 0;
}

static uint64_t synopsys_usb_read(void *opaque, hwaddr _addr, unsigned size)
{
	synopsys_usb_state *state = (synopsys_usb_state *)opaque;

	if (synopsys_usb_trace_enabled())
		fprintf(stderr, "[USBTRACE] R 0x%04x (size %u)\n", (unsigned)_addr, size);

	switch(_addr)
	{
	case PCGCCTL:
		return state->pcgcctl;

	case GOTGCTL:
		return state->gotgctl;

	case GOTGINT:
		return state->gotgint;

	case GRSTCTL:
		return state->grstctl;

	case GHWCFG1:
		return state->ghwcfg1;

	case GHWCFG2:
		return state->ghwcfg2;

	case GHWCFG3:
		return state->ghwcfg3;

	case GHWCFG4:
		return state->ghwcfg4;

	case GAHBCFG:
		return state->gahbcfg;

	case GUSBCFG:
		return state->gusbcfg;

	case GINTMSK:
		return state->gintmsk;

	case GINTSTS:
		return state->gintsts;

	case DIEPMSK:
		return state->diepmsk;

	case DOEPMSK:
		return state->doepmsk;

	case DAINTMSK:
		return state->daintmsk;
	
	case DAINTSTS:
		return state->daintsts;

	case DCTL:
		return state->dctl;

	case DCFG:
		return state->dcfg;

	case DSTS:
		return state->dsts;

	case GRXSTSR:
	case GRXSTSP:
		return 0; // TODO: Do something about this?

	case GNPTXFSTS:
		return 0xFFFFFFFF;

	case GRXFSIZ:
		return state->grxfsiz;

	case GNPTXFSIZ:
		return state->gnptxfsiz;

	case DIEPTXF(1) ... DIEPTXF(USB_NUM_FIFOS+1):
		_addr -= DIEPTXF(1);
		_addr >>= 2;
		return state->dptxfsiz[_addr];

	case USB_INREGS ... (USB_INREGS + USB_EPREGS_SIZE - 4):
		_addr -= USB_INREGS;
		return synopsys_usb_in_ep_read(state, _addr >> 5, _addr & 0x1f);

	case USB_OUTREGS ... (USB_OUTREGS + USB_EPREGS_SIZE - 4):
		_addr -= USB_OUTREGS;
		return synopsys_usb_out_ep_read(state, _addr >> 5, _addr & 0x1f);

	case USB_FIFO_START ... USB_FIFO_END-4:
		_addr -= USB_FIFO_START;
		return *((uint32_t*)(&state->fifos[_addr]));

	default:
		qemu_log_mask(LOG_UNIMP, "usb_synopsys: unimplemented read  0x%04x (size %u)\n",
		              (unsigned)_addr, size);
	}

	return 0;
}

static void synopsys_usb_in_ep_write(synopsys_usb_state *_state, int _ep, hwaddr _addr, uint32_t _val)
{
	if(_ep >= USB_NUM_ENDPOINTS)
	{
		qemu_log_mask(LOG_UNIMP, "usb_synopsys: write to out-of-range IN EP %d\n", _ep);
		return;
	}

    switch (_addr)
	{
    case 0x00:
		_state->in_eps[_ep].control = _val;
		if (_ep && synopsys_usb_in_debug() && (_val & USB_EPCON_ENABLE)) {
			fprintf(stderr, "[USBIN] arm ep%d DIEPCTL=0x%08x dma=0x%08x tsiz=0x%08x "
			        "(xfer=%u pktcnt=%u)\n", _ep, (uint32_t)_val,
			        (uint32_t)_state->in_eps[_ep].dma_address,
			        _state->in_eps[_ep].tx_size,
			        (unsigned)(_state->in_eps[_ep].tx_size & DEPTSIZ_XFERSIZ_MASK),
			        (unsigned)((_state->in_eps[_ep].tx_size >> DEPTSIZ_PKTCNT_SHIFT)
			                   & DEPTSIZ_PKTCNT_MASK));
		}
		synopsys_usb_update_in_ep(_state, _ep);
		return;

    case 0x08:
        _state->in_eps[_ep].interrupt_status &=~ _val;
		synopsys_usb_update_irq(_state);
		return;

    case 0x10:
        _state->in_eps[_ep].tx_size = _val;
		return;

    case 0x14:
        _state->in_eps[_ep].dma_address = _val;
		return;

    case 0x1C:
        _state->in_eps[_ep].dma_buffer = _val;
		return;

    default:
        qemu_log_mask(LOG_UNIMP, "usb_synopsys: unimplemented IN EP%d write offset 0x"
                      HWADDR_FMT_plx " val 0x%08x\n", _ep, _addr, _val);
		break;
    }
}

static void synopsys_usb_out_ep_write(synopsys_usb_state *_state, int _ep, hwaddr _addr, uint32_t _val)
{
	if(_ep >= USB_NUM_ENDPOINTS)
	{
		qemu_log_mask(LOG_UNIMP, "usb_synopsys: write to out-of-range OUT EP %d\n", _ep);
		return;
	}

    switch (_addr)
	{
	case 0x00:
        _state->out_eps[_ep].control = _val;
		if (synopsys_usb_trace_enabled()) {
			fprintf(stderr, "[USBDMA] guest DOEPCTL[%d] = 0x%08x (dma 0x%08x tsiz 0x%08x)\n",
			        _ep, (uint32_t)_val, _state->out_eps[_ep].dma_address,
			        _state->out_eps[_ep].tx_size);
		}
		synopsys_usb_update_out_ep(_state, _ep);
		return;

    case 0x08:
        _state->out_eps[_ep].interrupt_status &=~ _val;
		synopsys_usb_update_irq(_state);
		return;

    case 0x10:
        _state->out_eps[_ep].tx_size = _val;
		return;

    case 0x14:
		if (synopsys_usb_trace_enabled()) {
			fprintf(stderr, "[USBDMA] guest DOEPDMA[%d] = 0x%08x (was 0x%08x)\n",
			        _ep, (uint32_t)_val, _state->out_eps[_ep].dma_address);
		}
        _state->out_eps[_ep].dma_address = _val;
		return;

    case 0x1C:
        _state->out_eps[_ep].dma_buffer = _val;
		return;

    default:
        qemu_log_mask(LOG_UNIMP, "usb_synopsys: unimplemented OUT EP%d write offset 0x"
                      HWADDR_FMT_plx " val 0x%08x\n", _ep, _addr, _val);
		break;
    }
}

static void synopsys_usb_write(void *opaque, hwaddr _addr, uint64_t _val, unsigned size)
{
	synopsys_usb_state *state = (synopsys_usb_state *)opaque;

	if (synopsys_usb_trace_enabled())
		fprintf(stderr, "[USBTRACE] W 0x%04x = 0x%08x (size %u)\n",
		        (unsigned)_addr, (unsigned)_val, size);

	switch(_addr)
	{
	case PCGCCTL:
		state->pcgcctl = _val;
		synopsys_usb_update_irq(state);
		return;

	case GOTGCTL:
		state->gotgctl = _val;
		break;

	case GOTGINT:
		state->gotgint &=~ _val;
		synopsys_usb_update_irq(state);
		return;

	case GRSTCTL:
		if(_val & GRSTCTL_CORESOFTRESET)
		{
			state->grstctl = GRSTCTL_CORESOFTRESET;

			/*
			 * The original connected to the host here and hw_error()'d if that
			 * failed, so a missing host bridge killed the VM mid-boot. The
			 * connection is established at realize now (see
			 * synopsys_usb_tcp_start); a core soft reset just retries if we are
			 * not connected yet.
			 */
			synopsys_usb_tcp_start(state);

			state->grstctl &= ~GRSTCTL_CORESOFTRESET;
			state->grstctl |= GRSTCTL_AHBIDLE;
			state->gintsts |= GINTMSK_RESET;
			synopsys_usb_update_irq(state);
		}
		else if(_val == 0)
			state->grstctl = _val;

		return;

	case GINTMSK:
		state->gintmsk = _val;
		synopsys_usb_update_irq(state);
		break;

	case GINTSTS:
		state->gintsts &=~ _val;
		synopsys_usb_update_irq(state);
		return;

	case DOEPMSK:
		state->doepmsk = _val;
		synopsys_usb_update_irq(state);
		return;

	case DIEPMSK:
		state->diepmsk = _val;
		synopsys_usb_update_irq(state);
		return;

	case DAINTMSK:
		state->daintmsk = _val;
		synopsys_usb_update_irq(state);
		return;
	
	case DAINTSTS:
		state->daintsts &=~ _val;
		synopsys_usb_update_irq(state);
		return;

	case GAHBCFG:
		state->gahbcfg = _val;
		return;

	case GUSBCFG:
		state->gusbcfg = _val;
		return;

	case DCTL:
		if((_val & DCTL_SGNPINNAK) != (state->dctl & DCTL_SGNPINNAK)
				&& (_val & DCTL_SGNPINNAK))
		{
			state->gintsts |= GINTMSK_GINNAKEFF;
			_val &=~ DCTL_SGNPINNAK;
		}

		if((_val & DCTL_SGOUTNAK) != (state->dctl & DCTL_SGOUTNAK)
				&& (_val & DCTL_SGOUTNAK))
		{
			state->gintsts |= GINTMSK_GOUTNAKEFF;
			_val &=~ DCTL_SGOUTNAK;
		}

		state->dctl = _val;
		synopsys_usb_update_irq(state);
		return;

	case DCFG:
		//printf("USB: dcfg = 0x%08x.\n", _val);
		state->dcfg = _val;
		return;

	case GRXFSIZ:
		state->grxfsiz = _val;
		return;

	case GNPTXFSIZ:
		state->gnptxfsiz = _val;
		return;

	case DIEPTXF(1) ... DIEPTXF(USB_NUM_FIFOS+1):
		_addr -= DIEPTXF(1);
		_addr >>= 2;
		state->dptxfsiz[_addr] = _val;
		return;

	case USB_INREGS ... (USB_INREGS + USB_EPREGS_SIZE - 4):
		_addr -= USB_INREGS;
		synopsys_usb_in_ep_write(state, _addr >> 5, _addr & 0x1f, _val);
		return;

	case USB_OUTREGS ... (USB_OUTREGS + USB_EPREGS_SIZE - 4):
		_addr -= USB_OUTREGS;
		synopsys_usb_out_ep_write(state, _addr >> 5, _addr & 0x1f, _val);
		return;

	case USB_FIFO_START ... USB_FIFO_END-4:
		_addr -= USB_FIFO_START;
		*((uint32_t*)(&state->fifos[_addr])) = _val;
		return;

	default:
		qemu_log_mask(LOG_UNIMP, "usb_synopsys: unimplemented write 0x%04x = 0x%08x (size %u)\n",
		              (unsigned)_addr, (unsigned)_val, size);
	}
}

static const MemoryRegionOps usb_otg_ops = {
    .read = synopsys_usb_read,
    .write = synopsys_usb_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void s5l8900_usb_otg_reset(DeviceState *d)
{
	synopsys_usb_state *state = S5L8900USBOTG(d);

	printf("USB: cfg = 0x%08x, 0x%08x, 0x%08x, 0x%08x.\n",
			state->ghwcfg1,
			state->ghwcfg2,
			state->ghwcfg3,
			state->ghwcfg4);

	state->pcgcctl = 3;

	state->gahbcfg = 0;
	state->gusbcfg = 0;

	state->dctl = 0;
	state->dcfg = 0;
	state->dsts = 0;

	state->gotgctl = 0;
	state->gotgint = 0;

	/*
	 * The AHB master is always idle here - there is no bus to be busy on.
	 * grstctl was never initialised, so AHBIDLE read as clear forever and
	 * AppleSynopsysOTG2::_coreInit panicked with "AHB not idle"
	 * (AppleSynopsysOTG2.cpp:394) while polling this register.
	 */
	state->grstctl = GRSTCTL_AHBIDLE;

	state->gintmsk = 0;
	state->gintsts = 0;

	state->daintmsk = 0;
	state->daintsts = 0;

	state->diepmsk = 0;
	state->doepmsk = 0;

	state->grxfsiz = 0x100;
	state->gnptxfsiz = (0x100 << 16) | 0x100;

	uint32_t counter = 0x200;
	int i;
	for(i = 0; i < USB_NUM_FIFOS; i++)
	{
		state->dptxfsiz[i] = (counter << 16) | 0x100;
		counter += 0x100;
	}

	for(i = 0; i < USB_NUM_ENDPOINTS; i++)
	{
		/* interrupt_status was the one field left standing: DIEPINT/DOEPINT
		 * kept the previous boot's bits, so the first diepmsk/doepmsk the new
		 * driver writes would immediately re-raise an interrupt for a transfer
		 * that completed before the reset. */
		synopsys_usb_ep_state *in = &state->in_eps[i];
		in->control = 0;
		in->dma_address = 0;
		in->fifo = 0;
		in->tx_size = 0;
		in->interrupt_status = 0;

		synopsys_usb_ep_state *out = &state->out_eps[i];
		out->control = 0;
		out->dma_address = 0;
		out->fifo = 0;
		out->tx_size = 0;
		out->interrupt_status = 0;
	}

	synopsys_usb_update_irq(state);
}

static void s5l8900_usb_otg_init1(Object *obj)
{
	DeviceState *dev = DEVICE(obj);
    synopsys_usb_state *s = S5L8900USBOTG(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    /*
     * The region must cover the FIFO window at USB_FIFO_START (0x1000) as well
     * as the register block; it was previously sized 0x1000, so every FIFO
     * access fell outside the device entirely and the FIFO cases in
     * synopsys_usb_read/write were unreachable. That also hides slave-mode
     * (non-DMA) traffic, which Phase 0 needs to see.
     */
    memory_region_init_io(&s->iomem, OBJECT(s), &usb_otg_ops, s, "usb_otg", USB_FIFO_END);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
}

// Helper for adding to a machine
DeviceState *ipod_touch_init_usb_otg(qemu_irq _irq, uint32_t _hwcfg[4])
{
	DeviceState *dev = qdev_new(TYPE_S5L8900USBOTG);
	synopsys_usb_state *state = S5L8900USBOTG(dev);

	state->ghwcfg1 = _hwcfg[0];
	state->ghwcfg2 = _hwcfg[1];
	state->ghwcfg3 = _hwcfg[2];
	state->ghwcfg4 = _hwcfg[3];

	SysBusDevice *sdev = SYS_BUS_DEVICE(dev);
    sysbus_connect_irq(sdev, 0, _irq);

    return dev;
}

static void s5l8900_usb_otg_realize(DeviceState *dev, Error **errp)
{
    /* Dial the host bridge once, if one is configured. A core soft reset
     * retries, so the bridge may also be started after the guest is up. */
    synopsys_usb_tcp_start(S5L8900USBOTG(dev));
}

static const VMStateDescription vmstate_synopsys_usb_ep = {
    .name = "synopsys_usb_ep",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, synopsys_usb_ep_state),
        VMSTATE_UINT32(tx_size, synopsys_usb_ep_state),
        VMSTATE_UINT32(fifo, synopsys_usb_ep_state),
        VMSTATE_UINT32(interrupt_status, synopsys_usb_ep_state),
        VMSTATE_UINT64(dma_address, synopsys_usb_ep_state),
        VMSTATE_UINT64(dma_buffer, synopsys_usb_ep_state),
        VMSTATE_END_OF_LIST()
    }
};

/*
 * KNOWN GAP, deliberate: tcp_state is a live socket to usbmuxd and cannot be
 * migrated. A restored snapshot comes up with the USB core's registers intact
 * but the host bridge disconnected -- the same situation as unplugging the
 * cable, which the guest already handles. ghwcfg1-4 are machine configuration
 * rather than state, but they are cheap and migrating them makes a mismatched
 * destination fail loudly instead of subtly.
 *
 * post_load re-drives the interrupt line from the restored masks and status,
 * for the same reason pl192 does: restoring gintsts is not the same as
 * asserting the line.
 */
static int synopsys_usb_post_load(void *opaque, int version_id)
{
    synopsys_usb_update_irq(opaque);
    return 0;
}

static const VMStateDescription vmstate_synopsys_usb = {
    .name = "synopsys_usb_otg",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = synopsys_usb_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(pcgcctl, synopsys_usb_state),
        VMSTATE_UINT32(ghwcfg1, synopsys_usb_state),
        VMSTATE_UINT32(ghwcfg2, synopsys_usb_state),
        VMSTATE_UINT32(ghwcfg3, synopsys_usb_state),
        VMSTATE_UINT32(ghwcfg4, synopsys_usb_state),
        VMSTATE_UINT32(gahbcfg, synopsys_usb_state),
        VMSTATE_UINT32(gusbcfg, synopsys_usb_state),
        VMSTATE_UINT32(grxfsiz, synopsys_usb_state),
        VMSTATE_UINT32(gnptxfsiz, synopsys_usb_state),
        VMSTATE_UINT32(gotgctl, synopsys_usb_state),
        VMSTATE_UINT32(gotgint, synopsys_usb_state),
        VMSTATE_UINT32(grstctl, synopsys_usb_state),
        VMSTATE_UINT32(gintmsk, synopsys_usb_state),
        VMSTATE_UINT32(gintsts, synopsys_usb_state),
        VMSTATE_UINT32_ARRAY(dptxfsiz, synopsys_usb_state, USB_NUM_FIFOS),
        VMSTATE_UINT32(dctl, synopsys_usb_state),
        VMSTATE_UINT32(dcfg, synopsys_usb_state),
        VMSTATE_UINT32(dsts, synopsys_usb_state),
        VMSTATE_UINT32(daintmsk, synopsys_usb_state),
        VMSTATE_UINT32(daintsts, synopsys_usb_state),
        VMSTATE_UINT32(diepmsk, synopsys_usb_state),
        VMSTATE_UINT32(doepmsk, synopsys_usb_state),
        VMSTATE_STRUCT_ARRAY(in_eps, synopsys_usb_state, USB_NUM_ENDPOINTS, 1,
                             vmstate_synopsys_usb_ep, synopsys_usb_ep_state),
        VMSTATE_STRUCT_ARRAY(out_eps, synopsys_usb_state, USB_NUM_ENDPOINTS, 1,
                             vmstate_synopsys_usb_ep, synopsys_usb_ep_state),
        VMSTATE_UINT8_ARRAY(fifos, synopsys_usb_state, 0x100 * (USB_NUM_FIFOS + 1)),
        VMSTATE_END_OF_LIST()
    }
};

static void s5l8900_usb_otg_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, s5l8900_usb_otg_reset);
    dc->vmsd = &vmstate_synopsys_usb;
    dc->realize = s5l8900_usb_otg_realize;
}

static const TypeInfo s5l8900_usb_otg_info = {
    .name          = TYPE_S5L8900USBOTG,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(synopsys_usb_state),
    .instance_init = s5l8900_usb_otg_init1,
    .class_init    = s5l8900_usb_otg_class_init,
};

static void s5l8900_usb_otg_register_types(void)
{
    type_register_static(&s5l8900_usb_otg_info);
}

type_init(s5l8900_usb_otg_register_types)
