/*
 * ARM PrimeCell PL080/PL081 DMA controller
 *
 * Copyright (c) 2006 CodeSourcery.
 * Copyright (c) 2018 Linaro Limited
 * Written by Paul Brook, Peter Maydell
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 or
 * (at your option) any later version.
 */

/*
 * This is a model of the Arm PrimeCell PL080/PL081 DMA controller:
 * The PL080 TRM is:
 * https://developer.arm.com/documentation/ddi0196/latest
 * and the PL081 TRM is:
 * https://developer.arm.com/documentation/ddi0218/latest
 *
 * QEMU interface:
 * + sysbus IRQ 0: DMACINTR combined interrupt line
 * + sysbus IRQ 1: DMACINTERR error interrupt request
 * + sysbus IRQ 2: DMACINTTC count interrupt request
 * + sysbus MMIO region 0: MemoryRegion for the device's registers
 * + QOM property "downstream": MemoryRegion defining where DMA
 *   bus master transactions are made
 */

#ifndef HW_DMA_PL080_H
#define HW_DMA_PL080_H

#include "hw/sysbus.h"
#include "qom/object.h"

#define PL080_MAX_CHANNELS 8

typedef struct {
    uint32_t src;
    uint32_t dest;
    uint32_t lli;
    uint32_t ctrl;
    uint32_t conf;
} pl080_channel;

#define TYPE_PL080 "pl080"
#define TYPE_PL081 "pl081"
OBJECT_DECLARE_SIMPLE_TYPE(PL080State, PL080)

struct PL080State {
    SysBusDevice parent_obj;

    MemoryRegion iomem1;
    MemoryRegion iomem2;
    uint8_t tc_int;
    uint8_t tc_mask;
    uint8_t err_int;
    uint8_t err_mask;
    uint32_t conf;
    uint32_t sync;
    uint32_t req_single;
    uint32_t req_burst;
    /*
     * Peripheral request lines that a device model actually drives (see
     * pl080_attach_paced_peripheral). Only those are allowed to gate a
     * memory<->peripheral transfer; every other peripheral keeps the historical
     * "transfer the whole descriptor the instant the channel is enabled"
     * behaviour, because nothing drives its request line and gating it would
     * stall the channel forever.
     */
    uint32_t paced_req;
    pl080_channel chan[PL080_MAX_CHANNELS];
    int nchannels;
    /* Flag to avoid recursive DMA invocations.  */
    int running;
    qemu_irq irq;
    qemu_irq interr;
    qemu_irq inttc;

    MemoryRegion *downstream;
    AddressSpace downstream_as;

    /* IT_DMAC_TRACE: which controller this is, in instantiation order, purely
     * so a trace of two identical devices can be told apart. */
    int trace_id;
    /* IT_DMAC_TRACE: last logged level of the combined interrupt line. */
    int last_level;
    /*
     * Source address of the element currently in flight, on whichever channel
     * is transferring. A destination peripheral sees only an MMIO write, so
     * without this it cannot tell which part of the guest's buffer a byte came
     * from -- which is exactly the question when that buffer is a circular
     * audio ring and the transfer laps it. Diagnostic only (IT_I2S_TRACE);
     * nothing depends on it and it is deliberately not migrated.
     */
    uint32_t paced_src;
};

/*
 * Opt a peripheral request line into real flow control.
 *
 * Call once at machine-init time, before any transfer. Until a peripheral says
 * "I drive my request line", pl080_run treats memory<->peripheral descriptors
 * as free-running and moves every byte inside the Config write that enables the
 * channel -- which is what made the iPod touch's I2S DMA copy an entire 72 KB
 * audio ring in zero guest time, long before anything had written PCM into it.
 */
void pl080_attach_paced_peripheral(PL080State *s, int id);

/*
 * Drive that request line. Raising it lets the channel run again; the model
 * re-reads the line between elements, so the peripheral can stop the burst as
 * soon as its FIFO is full.
 */
void pl080_set_dma_request(PL080State *s, int id, bool level);

/*
 * DMACLBREQ/DMACLSREQ: the peripheral's packet has ended. Moves whatever is
 * pending, then terminates the descriptor early and raises terminal count --
 * how a variable-length peripheral read (a UART receive) ever completes.
 */
void pl080_set_dma_last_request(PL080State *s, int id);

#endif
