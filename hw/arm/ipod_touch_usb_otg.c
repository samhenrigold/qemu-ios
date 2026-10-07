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
#include "qemu/timer.h"
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
	
	/*
	 * The core's interrupt line: a pending unmasked GINTSTS bit, with
	 * GAHBCFG.GlblIntrMsk set. It used to be gated on PCGCCTL instead (clocks
	 * running); iPhone OS 1.x never writes PCGCCTL -- iBoot-204 leaves it at 3
	 * (its USB quiesce) and AppleS5L8900XUSBWrangler runs the core from there
	 * -- so under that gate the 1G took no interrupt at all.
	 */
	if((_state->gahbcfg & GAHBCFG_MASKINT) && (_state->gintmsk & _state->gintsts))
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

	/* CNAK is write-only: it ends a NAK the core or the driver set. Only the
	 * IN side honours NAKSts (see the transport callback). */
	if(eps->control & USB_EPCON_CLEARNAK)
		eps->control &=~ (USB_EPCON_CLEARNAK | USB_EPCON_NAKSTS);
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

    /* A PHY held in reset cannot receive bus traffic or perform DMA. Keep
     * core registers and latched IRQs intact: this is not a core/bus reset,
     * and the host transport's capability hello is not a physical packet.
     * ORSTCON deassertion resumes transfers; no firmware address or delay is
     * involved. PCGCCTL alone is not used here (older guests leave it at 3).
     */
    if (state->phy_reset && !((_hdr->flags & tcp_usb_hello) &&
                              (_hdr->ep & 0x7f) == TCP_USB_EP_CONTROL)) {
        return USB_RET_NAK;
    }

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

	/*
	 * A port that supplies no charge current (a 500 mA port, a hub that cannot
	 * charge): the bridge's Apple vendor power request (bmRequestType 0x40,
	 * bRequest 0x40) never reaches the guest, as when the host refuses it, so
	 * the iPad stays at 500 mA, "Not Charging", while data flows. The host sees
	 * it fail, which usbmuxd takes as "the device keeps 500 mA".
	 */
	if (state->withhold_charge && ep == 0 && (_hdr->flags & tcp_usb_setup) && hdr_len >= 2 &&
	    (uint8_t)_buffer[0] == 0x40 && (uint8_t)_buffer[1] == 0x40) {
		printf("[USBTCP] charge request withheld: the port supplies no charge current\n");
		return USB_RET_STALL;
	}

	if (_hdr->ep & USB_DIR_IN) {
		synopsys_usb_ep_state *eps = &state->in_eps[ep];

		if (eps->control & USB_EPCON_STALL) {
			eps->control &= ~USB_EPCON_STALL;
			ret = USB_RET_STALL;
		} else if (eps->control & USB_EPCON_NAKSTS) {
			/* NAKSts: the core NAKs IN tokens even with data armed. */
			ret = USB_RET_NAK;
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
			 * PktCnt counts down alongside XferSize on real hardware, EP0
			 * included, where the MPS field is a two-bit enum (0 = 64 bytes ...
			 * 3 = 8). iPhone OS 1.x's SynopsysHAL reads DIEPTSIZ0's PktCnt back
			 * after XferCompl and sent a control IN's first packet again while it
			 * stayed at 1: the 82-byte serial string reached the host as 64 + 64
			 * bytes of the same packet (usbmuxd read the UDID as '?').
			 */
			uint32_t mps = ep ? eps->control & USB_EPCON_MPS_MASK
			                  : 64u >> (eps->control & 3);
			if (mps && amtDone) {
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
			 * transfer-oriented, not packet-oriented. A zero-length transaction
			 * is the host's ZLP and completes an armed transfer with 0 bytes;
			 * the host sends one only when its software asks (usbmuxd does
			 * after a max-packet-multiple write, which iOS 4's mux needs to
			 * find the end of a message; libirecovery's uploads do not).
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

			/*
			 * As the core does: a bulk OUT transfer ends on a short packet (a
			 * ZLP included) or once its XferSize or PktCnt runs out. A
			 * max-packet-multiple transaction short of both leaves the endpoint
			 * enabled, its DMA address and counts advanced, and raises nothing.
			 * 1.x's AppleUSBDeviceMux arms 32 KiB and re-arms after a completion
			 * that looks like a full packet (by PktCnt): a 512-byte mux packet
			 * and the next one arrived as one 540-byte packet
			 * ("expected 512 bytes, received 540"), the TCP stream lost 484
			 * bytes and every lockdown session with a full-packet record died.
			 */
			uint32_t mps = eps->control & USB_EPCON_MPS_MASK;
			uint32_t pkts = (eps->tx_size >> DEPTSIZ_PKTCNT_SHIFT) & DEPTSIZ_PKTCNT_MASK;
			if (ep != 0 && !(_hdr->flags & tcp_usb_setup) && mps && amtDone
			    && amtDone % mps == 0 && amtDone < sz && amtDone / mps < pkts) {
				eps->control |= USB_EPCON_ENABLE;
				eps->tx_size = (eps->tx_size
				                & ~(DEPTSIZ_XFERSIZ_MASK | (DEPTSIZ_PKTCNT_MASK << DEPTSIZ_PKTCNT_SHIFT)))
				             | ((sz - amtDone) & DEPTSIZ_XFERSIZ_MASK)
				             | ((pkts - amtDone / mps) << DEPTSIZ_PKTCNT_SHIFT);
				if (synopsys_usb_trace_enabled())
					fprintf(stderr, "[USBTCP] OUT ep%d %zu bytes (transfer continues)\n", ep, amtDone);
				synopsys_usb_update_irq(state);
				return amtDone;
			}

			if (_hdr->flags & tcp_usb_setup) {
				eps->interrupt_status |= USB_EPINT_SetUp;
				/*
				 * A SETUP ends whatever control transfer came before it. The
				 * core clears EP0's STALL and sets DIEPCTL0's NAK, so a data
				 * stage the driver armed for an earlier request -- one the
				 * host gave up on before a bus reset -- is NAKed instead of
				 * answering this one, until the driver arms the new reply
				 * with CNAK. Without it the pipe ran one reply behind:
				 * usbmuxd read device-descriptor bytes as a configuration
				 * header ("Short configuration 0 (-1 of 512)").
				 */
				if (ep == 0) {
					state->in_eps[0].control = (state->in_eps[0].control
					                            & ~USB_EPCON_STALL) | USB_EPCON_NAKSTS;
					eps->control &= ~USB_EPCON_STALL;
				}
			} else {
				eps->interrupt_status |= USB_EPINT_XferCompl;
			}

			eps->tx_size = (eps->tx_size & ~DEPTSIZ_XFERSIZ_MASK)
			             | ((sz - amtDone) & DEPTSIZ_XFERSIZ_MASK);
			/*
			 * PktCnt counts the packets received, a ZLP or a short packet too:
			 * 1.x tells a short-packet end from a full-packet one by it (bytes
			 * versus packets x MPS) and re-armed after a ZLP it could not see.
			 */
			if (ep != 0 && mps && !(_hdr->flags & tcp_usb_setup)) {
				uint32_t used = MIN(pkts, MAX(1u, (uint32_t)DIV_ROUND_UP(amtDone, mps)));
				eps->tx_size = (eps->tx_size & ~(DEPTSIZ_PKTCNT_MASK << DEPTSIZ_PKTCNT_SHIFT))
				             | ((pkts - used) << DEPTSIZ_PKTCNT_SHIFT);
			}

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

/* "host:port" of the host bridge (usbmuxd-qemu); the port defaults to 1235,
 * an empty host to 127.0.0.1. An absolute path is the bridge's Unix socket.
 * NULL or "" leaves the link unconfigured. */
void synopsys_usb_set_tcp_addr(synopsys_usb_state *state, const char *spec)
{
	if (!spec || !*spec) {
		return;
	}
	if (spec[0] == '/') {
		g_free(state->server_host);
		state->server_host = g_strdup(spec);
		state->server_port = 0;
		return;
	}
	g_autofree char *dup = g_strdup(spec);
	char *colon = strrchr(dup, ':');
	state->server_port = 0;
	if (colon) {
		*colon = '\0';
		state->server_port = atoi(colon + 1);
	}
	if (!state->server_port) {
		state->server_port = 1235;
	}
	g_free(state->server_host);
	state->server_host = g_strdup(*dup ? dup : "127.0.0.1");
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
	if (!_state->cable_attached ||
	    (_state->tcp_connected && !tcp_usb_closed(&_state->tcp_state))) {
		return;
	}

	if (!_state->server_host) {
		synopsys_usb_set_tcp_addr(_state, getenv("IT_USB_TCP"));
		if (!_state->server_host) {
			return;
		}
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

static uint64_t synopsys_usb_read_reg(void *opaque, hwaddr _addr, unsigned size);

/* Trace reads with the value returned, so a driver's decision can be read off the log. */
static uint64_t synopsys_usb_read(void *opaque, hwaddr _addr, unsigned size)
{
	uint64_t v = synopsys_usb_read_reg(opaque, _addr, size);
	if (synopsys_usb_trace_enabled())
		fprintf(stderr, "[USBTRACE] R 0x%04x -> 0x%08x (size %u)\n", (unsigned)_addr, (unsigned)v, size);
	return v;
}

static uint64_t synopsys_usb_read_reg(void *opaque, hwaddr _addr, unsigned size)
{
	synopsys_usb_state *state = (synopsys_usb_state *)opaque;

	switch(_addr)
	{
	case PCGCCTL:
		return state->pcgcctl;

	case GOTGCTL:
		/* Live status: the ID pin of a device cable (B-device) and, while a host
		 * powers VBUS, both session-valid comparators. */
		return (state->gotgctl & ~(GOTGCTL_CONIDSTS | GOTGCTL_ASESSIONVALID | GOTGCTL_BSESSIONVALID)) |
		       GOTGCTL_CONIDSTS |
		       (state->cable_attached ? GOTGCTL_ASESSIONVALID | GOTGCTL_BSESSIONVALID : 0);

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

	/*
	 * Inclusive case range, so ...DIEPTXF(USB_NUM_FIFOS+1) covered one register
	 * too many: (_addr - DIEPTXF(1)) >> 2 then yielded 0..16 into
	 * dptxfsiz[USB_NUM_FIFOS], which is 16 entries. Index 16 aliased the next
	 * struct member (dctl), so a write to a 17th FIFO register silently
	 * rewrote the device control register. No driver programs one today.
	 */
	case DIEPTXF(1) ... DIEPTXF(USB_NUM_FIFOS):
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
		/* NAKSts is read-only: only SNAK, CNAK and the core change it. */
		_state->in_eps[_ep].control = (_val & ~USB_EPCON_NAKSTS)
		                            | (_state->in_eps[_ep].control & USB_EPCON_NAKSTS);
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
			synopsys_usb_host_rearm(state, 2000);

			state->grstctl &= ~GRSTCTL_CORESOFTRESET;
			state->grstctl |= GRSTCTL_AHBIDLE;
			state->gintsts |= GINTMSK_RESET | GINTMSK_CONIDSTSCHNG;
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
		synopsys_usb_update_irq(state);
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

		/* The NAK-effective interrupts stay up while the global NAK is in
		 * effect and drop when the driver clears it: iOS 4's driver writes
		 * CGNPINNAK/CGOUTNAK and polls GINTSTS for that (3.x's clears them
		 * in GINTSTS, which still works). */
		if(_val & DCTL_CGNPINNAK)
			state->gintsts &=~ GINTMSK_GINNAKEFF;
		if(_val & DCTL_CGOUTNAK)
			state->gintsts &=~ GINTMSK_GOUTNAKEFF;
		_val &=~ (DCTL_CGNPINNAK | DCTL_CGOUTNAK);

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

	/*
	 * Inclusive case range, so ...DIEPTXF(USB_NUM_FIFOS+1) covered one register
	 * too many: (_addr - DIEPTXF(1)) >> 2 then yielded 0..16 into
	 * dptxfsiz[USB_NUM_FIFOS], which is 16 entries. Index 16 aliased the next
	 * struct member (dctl), so a write to a 17th FIFO register silently
	 * rewrote the device control register. No driver programs one today.
	 */
	case DIEPTXF(1) ... DIEPTXF(USB_NUM_FIFOS):
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
	/* ConIDStsChng is set out of reset (the core's reset value): 1.x's
	 * AppleS5L8900XUSBDevice unmasks only it (and ModeMis) after core init and
	 * reads the ID status off GOTGCTL before it brings device mode up. */
	state->gintsts = GINTMSK_CONIDSTSCHNG;

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

	/*
	 * Drop the host link so the reset looks like a cable replug.
	 *
	 * Everything above returns the core to its power-on state, but the TCP
	 * connection to usbmuxd used to survive untouched -- and usbmuxd had no way
	 * to notice. Its device record stayed alive (no EOF on the socket), the
	 * bulk IN poll just NAKed forever, and NAK is not one of the codes that
	 * clears `alive`, so the device was never reaped or re-enumerated. After a
	 * guest reboot or Device > Restart the daemon therefore held a permanently
	 * dead device: the app still reported "Running", deviceReady() still said
	 * yes (idevice_new succeeds against the stale record), and every install or
	 * list hung until its own deadline with no recovery short of quitting.
	 *
	 * Closing it here gives usbmuxd the EOF it needs to reap and re-enumerate,
	 * and the 3 s retry tick below redials once the guest re-arms the core --
	 * which is exactly the replug this transport already knows how to handle.
	 */
	if (state->tcp_connected) {
		tcp_usb_cleanup(&state->tcp_state);
		state->tcp_connected = false;
	}

	synopsys_usb_update_irq(state);
}

static void synopsys_usb_phy_reset(void *opaque, int n, int level)
{
    synopsys_usb_state *state = opaque;
    state->phy_reset = level;
}

static void s5l8900_usb_otg_init1(Object *obj)
{
	DeviceState *dev = DEVICE(obj);
    synopsys_usb_state *s = S5L8900USBOTG(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    s->cable_attached = true;
    qdev_init_gpio_in_named(dev, synopsys_usb_phy_reset, "phy-reset", 1);

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

/*
 * Redial the host bridge whenever the link is down. A core soft reset also
 * retries, but the guest only resets the core around its own USB state
 * changes -- if usbmuxd drops the connection while the guest is up and idle
 * (its socket timeout misreading a busy guest as a wedged one, say), nothing
 * ever redials and the device is stranded until reboot. usbmuxd answers a
 * fresh connection by re-enumerating, which the guest experiences as the
 * cable being replugged.
 */
#define TCP_USB_RETRY_MS 3000

/*
 * Built-in host, used when no host bridge is configured. iOS only reports
 * "external power" and disables idle sleep once a host has configured the
 * device (AppleD1815PMUPowerSource wants >= 500 mA from the usb_500_100
 * function, which AppleSynopsysOTGDevice reports after SET_CONFIGURATION);
 * without one the iPad deep-sleeps a few minutes after SpringBoard. This
 * plays the part usbmuxd-qemu's usb-qemu.c plays: reset, enumdone, device
 * descriptor, SET_ADDRESS 1, configuration descriptors, then SET_CONFIGURATION
 * of the configuration carrying the AppleUSBMux interface (255/254/2), else
 * the last one. It issues one transaction per tick through the same entry
 * point as the bridge and treats NAK as "try again in a millisecond".
 */
enum {
	HP_IDLE, HP_RESET, HP_ENUMDONE, HP_DEV_SETUP, HP_DEV_IN, HP_DEV_STATUS,
	HP_ADDR_SETUP, HP_ADDR_STATUS, HP_CFG_HDR_SETUP, HP_CFG_HDR_IN,
	HP_CFG_HDR_STATUS, HP_CFG_SETUP, HP_CFG_IN, HP_CFG_STATUS,
	HP_SETCFG_SETUP, HP_SETCFG_STATUS, HP_CHARGE_SETUP, HP_CHARGE_STATUS,
	HP_STR0_SETUP, HP_STR0_IN, HP_STR0_STATUS,
	HP_STRSER_SETUP, HP_STRSER_IN, HP_STRSER_STATUS, HP_POLL, HP_DONE,
};
#define HOST_POLL_MS      5

#define HOST_RETRY_MS     1
#define HOST_TIMEOUT_MS   4000
#define HOST_RESTART_MS   3000

static int synopsys_host_xfer(synopsys_usb_state *s, uint8_t ep, uint8_t flags,
                              int len, void *buf)
{
	tcp_usb_header_t hdr = { .addr = 0, .ep = ep, .flags = flags, .length = len };

	return synopsys_usb_tcp_callback(NULL, s, &hdr, buf);
}

static int synopsys_host_setup(synopsys_usb_state *s, uint8_t type, uint8_t req,
                               uint16_t val, uint16_t idx, uint16_t len)
{
	uint8_t setup[8] = { type, req, val, val >> 8, idx, idx >> 8, len, len >> 8 };

	return synopsys_host_xfer(s, 0, tcp_usb_setup, 8, setup);
}

/* Configuration with the mux interface (255/254/2), else the last one; the
 * interface's bulk IN endpoint is what a host keeps polling afterwards. */
static bool synopsys_host_cfg_has_mux(synopsys_usb_state *s, const uint8_t *d, int n)
{
	bool in_mux = false, found = false;

	for (int i = 0; i + 1 < n && d[i] >= 2; i += d[i]) {
		if (d[i + 1] == 4 && i + 8 < n) {
			in_mux = d[i + 5] == 255 && d[i + 6] == 254 && d[i + 7] == 2;
			found |= in_mux;
		} else if (d[i + 1] == 5 && i + 3 < n && in_mux) {
			if (d[i + 2] & USB_DIR_IN) {
				s->host_ep_in = d[i + 2];
			} else {
				s->host_ep_out = d[i + 2];
			}
		}
	}
	return found;
}

static void synopsys_host_go(synopsys_usb_state *s, int phase, int64_t ms)
{
	s->host_phase = phase;
	s->host_deadline = qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + HOST_TIMEOUT_MS;
	timer_mod(s->host_timer, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + ms);
}

static void synopsys_host_tick(void *opaque)
{
	synopsys_usb_state *s = opaque;
	int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
	int r;

	if (!s->builtin_host || !s->cable_attached || s->host_phase == HP_IDLE ||
	    s->host_phase == HP_DONE) {
		return;
	}
	if (now > s->host_deadline) {
		if (synopsys_usb_trace_enabled())
			fprintf(stderr, "[USBHOST] phase %d timed out; restarting\n", s->host_phase);
		synopsys_host_go(s, HP_RESET, HOST_RESTART_MS);
		return;
	}

	switch (s->host_phase) {
	case HP_RESET:
		s->host_greeted = false;
		s->host_ep_in = s->host_ep_out = 0;
		synopsys_host_xfer(s, 0, tcp_usb_reset, 0, NULL);
		synopsys_host_go(s, HP_ENUMDONE, 500);
		return;
	case HP_ENUMDONE:
		synopsys_host_xfer(s, 0, tcp_usb_enumdone, 0, NULL);
		synopsys_host_go(s, HP_DEV_SETUP, 500);
		return;
	case HP_DEV_SETUP:
		r = synopsys_host_setup(s, 0x80, 6, 0x0100, 0, 18);
		s->host_got = 0; s->host_want = 18;
		break;
	case HP_CFG_HDR_SETUP:
		r = synopsys_host_setup(s, 0x80, 6, 0x0200 | s->host_cfg, 0, 9);
		s->host_got = 0; s->host_want = 9;
		break;
	case HP_CFG_SETUP:
		r = synopsys_host_setup(s, 0x80, 6, 0x0200 | s->host_cfg, 0, s->host_want);
		s->host_got = 0;
		break;
	case HP_ADDR_SETUP:
		r = synopsys_host_setup(s, 0x00, 5, 1, 0, 0);
		break;
	case HP_SETCFG_SETUP:
		r = synopsys_host_setup(s, 0x00, 9, s->host_cfg_value, 0, 0);
		break;
	case HP_CHARGE_SETUP:
		/*
		 * Apple's vendor power request, as a Mac's high-power port (or a
		 * charging dock) sends it: 500 mA base plus 1600 mA extra. The iPad's
		 * power source then reports "usb stack power 2100mA" and charges;
		 * without it a configured iPad stays at 500 mA, "Not Charging".
		 */
		r = synopsys_host_setup(s, 0x40, 0x40, 500, 1600, 0);
		break;
	case HP_STR0_SETUP:
		r = synopsys_host_setup(s, 0x80, 6, 0x0300, 0, 255);
		s->host_got = 0; s->host_want = 255;
		break;
	case HP_STRSER_SETUP:
		r = synopsys_host_setup(s, 0x80, 6, 0x0300 | s->host_iserial, 0x0409, 255);
		s->host_got = 0; s->host_want = 255;
		break;
	case HP_POLL:
		/*
		 * usbmuxd opens with a mux version request (v0 header: protocol 0,
		 * length 20; then major 2, minor 0, padding; all big-endian) and then
		 * never stops polling the bulk IN pipe. AppleUSBDeviceMux restarts the
		 * stack (core soft reset, interface deactivated) a few seconds after
		 * activation if the host stays silent, so do both; the reply and
		 * anything else the guest sends have no reader here and are dropped.
		 */
		if (!s->host_greeted) {
			static const uint8_t version_req[20] = {
				0, 0, 0, 0,  0, 0, 0, 20,  0, 0, 0, 2,  0, 0, 0, 0,  0, 0, 0, 0
			};
			uint8_t pkt[20];

			memcpy(pkt, version_req, sizeof(pkt));
			if (synopsys_host_xfer(s, s->host_ep_out ? s->host_ep_out : 0x02, 0,
			                       sizeof(pkt), pkt) >= 0) {
				s->host_greeted = true;
			}
		}
		synopsys_host_xfer(s, s->host_ep_in ? s->host_ep_in : 0x81, 0,
		                   sizeof(s->host_buf), s->host_buf);
		s->host_deadline = now + HOST_TIMEOUT_MS;
		timer_mod(s->host_timer, now + HOST_POLL_MS);
		return;
	case HP_DEV_IN:
	case HP_CFG_HDR_IN:
	case HP_CFG_IN:
	case HP_STR0_IN:
	case HP_STRSER_IN:
		r = synopsys_host_xfer(s, USB_DIR_IN, 0, s->host_want - s->host_got,
		                       s->host_buf + s->host_got);
		if (r > 0) {
			s->host_got += r;
			if (s->host_got < s->host_want && r == 64) {
				timer_mod(s->host_timer, now + HOST_RETRY_MS);
				return;                       /* more chunks to come */
			}
		}
		if (r == USB_RET_STALL && s->host_phase >= HP_STR0_IN) {
			r = 0;                            /* strings are optional */
		}
		break;
	case HP_DEV_STATUS:
	case HP_CFG_HDR_STATUS:
	case HP_CFG_STATUS:
	case HP_STR0_STATUS:
	case HP_STRSER_STATUS:
		r = synopsys_host_xfer(s, 0, 0, 0, NULL);          /* OUT status */
		break;
	case HP_ADDR_STATUS:
	case HP_SETCFG_STATUS:
	case HP_CHARGE_STATUS:
		r = synopsys_host_xfer(s, USB_DIR_IN, 0, 0, NULL);  /* IN status */
		break;
	default:
		return;
	}

	if (r == USB_RET_NAK) {
		timer_mod(s->host_timer, now + HOST_RETRY_MS);
		return;
	}
	if (r < 0) {
		if (synopsys_usb_trace_enabled())
			fprintf(stderr, "[USBHOST] phase %d failed (%d); restarting\n", s->host_phase, r);
		synopsys_host_go(s, HP_RESET, HOST_RESTART_MS);
		return;
	}

	/* Transaction accepted: advance. */
	switch (s->host_phase) {
	case HP_DEV_IN:
		if (s->host_got < 18 || s->host_buf[1] != 1) {
			synopsys_host_go(s, HP_RESET, HOST_RESTART_MS);
			return;
		}
		s->host_ncfg = s->host_buf[17];
		s->host_iserial = s->host_buf[16];
		s->host_cfg = 0;
		s->host_cfg_value = -1;
		break;
	case HP_CFG_HDR_IN:
		s->host_want = s->host_buf[2] | (s->host_buf[3] << 8);
		if (s->host_got < 9 || s->host_want < 9 || s->host_want > sizeof(s->host_buf)) {
			synopsys_host_go(s, HP_RESET, HOST_RESTART_MS);
			return;
		}
		break;
	case HP_CFG_IN:
		if (s->host_cfg_value < 0 || synopsys_host_cfg_has_mux(s, s->host_buf, s->host_got)) {
			s->host_cfg_value = s->host_buf[5];
		}
		break;
	case HP_CFG_STATUS:
		if (++s->host_cfg < s->host_ncfg &&
		    !synopsys_host_cfg_has_mux(s, s->host_buf, s->host_got)) {
			synopsys_host_go(s, HP_CFG_HDR_SETUP, HOST_RETRY_MS);
			return;
		}
		break;
	case HP_ADDR_STATUS:
		/* Let the guest program DCFG before the next SETUP, as usbmuxd does. */
		synopsys_host_go(s, HP_CFG_HDR_SETUP, 100);
		return;
	case HP_SETCFG_STATUS:
		printf("[USBHOST] built-in host: device configured (configuration value %d)\n",
		       s->host_cfg_value);
		if (!s->host_charge) {
			synopsys_host_go(s, HP_STR0_SETUP, HOST_RETRY_MS);
			return;
		}
		break;
	case HP_STRSER_STATUS:
		synopsys_host_go(s, HP_POLL, HOST_POLL_MS);
		return;
	default:
		break;
	}
	synopsys_host_go(s, s->host_phase + 1, HOST_RETRY_MS);
}

/* (Re)start enumeration after delay_ms: at realize, after a core soft reset
 * (the guest re-arms on every cable event) and on cable attach. */
void synopsys_usb_host_rearm(synopsys_usb_state *state, int64_t delay_ms)
{
	if (!state->builtin_host || !state->host_timer) {
		return;
	}
	if (!state->cable_attached) {
		state->host_phase = HP_IDLE;
		timer_del(state->host_timer);
		return;
	}
	synopsys_host_go(state, HP_RESET, delay_ms);
}

void synopsys_usb_set_cable(synopsys_usb_state *state, bool attached)
{
    state->cable_attached = attached;
    if (!attached && state->tcp_connected) {
        tcp_usb_cleanup(&state->tcp_state);
        state->tcp_connected = false;
    } else if (attached) {
        synopsys_usb_tcp_start(state);
    }
    synopsys_usb_host_rearm(state, 2000);
}

static void synopsys_usb_tcp_retry_tick(void *opaque)
{
    synopsys_usb_state *state = opaque;

    if (!state->tcp_connected || tcp_usb_closed(&state->tcp_state)) {
        synopsys_usb_tcp_start(state);
    }
    timer_mod(state->tcp_retry_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + TCP_USB_RETRY_MS);
}

static void s5l8900_usb_otg_realize(DeviceState *dev, Error **errp)
{
    synopsys_usb_state *state = S5L8900USBOTG(dev);

    /* Dial the host bridge once, if one is configured. A core soft reset
     * retries, so the bridge may also be started after the guest is up. */
    synopsys_usb_tcp_start(state);
    state->tcp_retry_timer = timer_new_ms(QEMU_CLOCK_REALTIME,
                                          synopsys_usb_tcp_retry_tick, state);
    timer_mod(state->tcp_retry_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + TCP_USB_RETRY_MS);
    state->host_timer = timer_new_ms(QEMU_CLOCK_REALTIME, synopsys_host_tick, state);
    synopsys_usb_host_rearm(state, 2000);
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
    synopsys_usb_state *state = opaque;
    /* The socket created by realize (or retained by an in-process load) has
     * no relationship to the restored endpoint transaction. Re-enumerate. */
    if (state->tcp_connected) {
        tcp_usb_cleanup(&state->tcp_state);
        state->tcp_connected = false;
    }
    timer_mod(state->tcp_retry_timer,
              qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + TCP_USB_RETRY_MS);
    /* Older streams stored a write's CNAK alongside NAKSts; settle it. */
    for (int i = 0; i < USB_NUM_ENDPOINTS; i++) {
        if (state->in_eps[i].control & USB_EPCON_CLEARNAK) {
            state->in_eps[i].control &= ~(USB_EPCON_CLEARNAK | USB_EPCON_NAKSTS);
        }
    }
    synopsys_usb_update_irq(state);
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
