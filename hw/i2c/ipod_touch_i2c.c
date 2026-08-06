/*
 *  IPod Touch I2C Bus Serial Interface Emulation
 *
 *  Copyright (C) 2012 Samsung Electronics Co Ltd.
 *    Maksim Kozlov, <m.kozlov@samsung.com>
 *    Igor Mitsyanko, <i.mitsyanko@samsung.com>
 *
 *  This program is free software; you can redistribute it and/or modify it
 *  under the terms of the GNU General Public License as published by the
 *  Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful, but WITHOUT
 *  ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 *  FITNESS FOR A PARTICULAR PURPOSE. See the GNU General Public License
 *  for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, see <http://www.gnu.org/licenses/>.
 *
 */

#include "hw/i2c/ipod_touch_i2c.h"
#include "migration/vmstate.h"

/*
 * Address-phase acknowledge.
 *
 * S5L8900_IICSTAT_LASTBIT is the "last bit received was 1", i.e. the slave did
 * NOT pull SDA low: a NAK. i2c_start_transfer() returns non-zero when no slave
 * on the bus matched the address, which is exactly that case.
 *
 * Until this existed the model threw the return value away and cleared the bit
 * unconditionally on every START, so an address nobody answers looked like a
 * successful transfer and the following reads returned QEMU's unmatched-bus
 * default of 0xff. Every absent device therefore presented as a present device
 * reporting all-ones -- which is how the missing CS42L58 codec was mistaken for
 * a misbehaving one, and it is latent for every other unmodelled I2C address.
 *
 * A NAK is still a completed address phase: the interrupt must still be raised
 * so the driver runs, inspects the status bit and returns an error, rather than
 * waiting for a transfer that will not come. Gated behind IT_I2C_NAK because it
 * changes what every existing guest sees -- iOS probes at least one address we
 * do not model (0x29, /device-tree/arm-io/i2c0/tethered), and that probe
 * currently "succeeds".
 */
static bool i2c_nak_enabled(void)
{
    static int on = -1;
    if (on < 0) {
        on = getenv("IT_I2C_NAK") != NULL;
    }
    return on;
}

/*
 * Whether any slave on this bus claims an address.
 *
 * The first version of this NAKed on i2c_start_transfer()'s return value, which
 * looked right and was not: both call sites re-derive the address from IICDS at
 * every START, including a REPEATED start. A register read is
 * START / addr+W / reg-index / repeated-START / addr+R, so at the repeated start
 * IICDS need not hold the address at all, the lookup misses, and a device that
 * is genuinely present gets NAKed. That is not theoretical - with the flag on,
 * the D1759 PMU could no longer read or write its own registers (19 read and 35
 * write failures in one boot) and USB cable detection stopped entirely, which is
 * strictly worse than the bug this was meant to fix.
 *
 * Asking the bus which addresses are claimed cannot misfire that way: it is a
 * property of the machine's topology, not of what happens to be in a register
 * mid-transaction. An address no device claims is genuinely absent no matter
 * which phase of a transfer we are in.
 */
static bool i2c_addr_is_claimed(IPodTouchI2CState *s, uint8_t addr)
{
    BusChild *kid;

    QTAILQ_FOREACH(kid, &BUS(s->bus)->children, sibling) {
        I2CSlave *slave = I2C_SLAVE(kid->child);

        if (slave->address == addr) {
            return true;
        }
    }
    return false;
}

/*
 * The address this START is really for.
 *
 * A register read is START / addr+W / reg-index / repeated-START / addr+R, and
 * this controller does not re-present the address on the repeated START - IICDS
 * still holds the register index. Deriving the address from IICDS every time
 * therefore looks up a register number as if it were a bus address: reading PMU
 * register 0x5c produced a lookup of 0x5c >> 1 = 0x2e, which no device claims,
 * so the PMU got NAKed on every read while writes (which issue no repeated
 * START) were fine. That is exactly the 108-failures-all-at-addr-5c signature.
 *
 * s->active is still set from the previous START when a repeated START arrives,
 * and is cleared at STOP, so it distinguishes the two cases.
 */
static uint8_t s5l8900_i2c_start_addr(IPodTouchI2CState *s)
{
    if (!s->active) {
        s->cur_addr = s->data >> 1;   /* fresh transaction: IICDS holds the address */
    }
    return s->cur_addr;
}

static void s5l8900_i2c_set_ack(IPodTouchI2CState *s, uint8_t addr)
{
    if (!i2c_nak_enabled()) {
        return;   /* legacy behaviour: every address appears to ACK */
    }
    if (!i2c_addr_is_claimed(s, addr)) {
        /*
         * NAK, but leave s->active alone. A NAK is a completed address phase:
         * the driver still needs its interrupt so it can read the status bit and
         * return an error. Tearing the transfer down here as well turned a
         * reported error into a wedged bus.
         */
        s->status |= S5L8900_IICSTAT_LASTBIT;
    } else {
        s->status &= ~S5L8900_IICSTAT_LASTBIT;
    }
}

static void s5l8900_i2c_update(IPodTouchI2CState *s)
{
    uint16_t level;
    level = (s->status & S5L8900_IICSTAT_START) &&
            (s->control & S5L8900_IICCON_IRQEN);

    if (s->control & S5L8900_IICCON_IRQPEND)
        level = 0;

    /*
     * UNCONDITIONAL, deliberately. `level` above is computed and thrown away,
     * which reads like a bug and is one on paper -- an unmasked-but-idle period
     * leaves this shared line asserted into the VIC. But switching to
     * qemu_set_irq(s->irq, level) HANGS THE BOOT: measured 2026-08-06, the
     * kernel loads and then never brings the UI up (framebuffer stuck on the
     * boot logo at lit=9852 for 900s). The guest's driver depends on getting an
     * interrupt here regardless of IRQPEND/IRQEN, and until someone maps what
     * AppleS5L8900X-I2C actually expects, the honest state is: known-imperfect,
     * known-working. Do not "clean this up" without a boot test.
     */
    (void)level;
    qemu_irq_raise(s->irq);
}

static int s5l8900_i2c_receive(IPodTouchI2CState *s)
{
    int r;
    r = i2c_recv(s->bus);
    s5l8900_i2c_update(s);
    return r;
}

static int s5l8900_i2c_send(IPodTouchI2CState *s, uint8_t data)
{
    if (!(s->status & S5L8900_IICSTAT_LASTBIT)) {
        s->status |= S5L8900_IICCON_ACKEN;
        s->data = data;
        s->iicreg20 |= 0x100;
        i2c_send(s->bus, s->data);
    }
    s5l8900_i2c_update(s);
    return 1;
}

/* I2C read function */
static uint64_t ipod_touch_i2c_read(void *opaque, hwaddr offset, unsigned size)
{
    IPodTouchI2CState *s = (IPodTouchI2CState *)opaque;

    //fprintf(stderr, "s5l8900_i2c_read(): offset = 0x%08x\n", offset);

    switch (offset) {
    case I2CCON:
        return s->control;
    case I2CSTAT:
        return s->status;
    case I2CADD:
        return s->address;
    case I2CDS:
        s->iicreg20 |= 0x100;
        s->data = s5l8900_i2c_receive(s);
        return s->data;
    case I2CLC:
        return s->line_ctrl;
    case IICREG20:
        {
            // clear the flags
            uint32_t tmp_reg20 = s->iicreg20;
            s->iicreg20 &= ~0x100;
            s->iicreg20 &= ~0x2000;
            return tmp_reg20; 
        }
    default:
        break;
        //hw_error("s5l8900.i2c: bad read offset 0x" TARGET_FMT_plx "\n", offset);
        //fprintf(stderr, "%s: bad read offset 0x%08x\n", __func__, offset);
    }
    return 0;
}

/* I2C write function */
static void ipod_touch_i2c_write(void *opaque, hwaddr offset, uint64_t value, unsigned size)
{
    IPodTouchI2CState *s = (IPodTouchI2CState *)opaque;
    int mode;

    //printf("s5l8900_i2c_write (base %d): offset = 0x%08x, val = 0x%08x\n", s->base, offset, value);

    qemu_irq_lower(s->irq);

    switch (offset) {
    case I2CCON:
        if(value & ~(S5L8900_IICCON_ACKEN)) {
            s->iicreg20 |= 0x100;
        }
        if((value & 0x10) && (s->status == 0x90))  {
            s->iicreg20 |= 0x2000;
        }
        s->control = value & 0xff;

        qemu_irq_raise(s->irq);
        break;

    case I2CSTAT:
        /* We have to make sure we don't miss an end transfer */
        if((!s->active) && ((s->status >> 6) != ((value >> 6)))) {
            /* LASTBIT reflects the bus, not the guest: it is status-only. */
            s->status = (value & 0xff & ~S5L8900_IICSTAT_LASTBIT) |
                        (s->status & S5L8900_IICSTAT_LASTBIT);
        /* If they toggle the tx bit then we have to force an end transfer before mode update */
        } else if((s->active) && ((s->status >> 6) != ((value >> 6)))) {
                    i2c_end_transfer(s->bus);
                    s->active=0;
                    s->status = (value & 0xff & ~S5L8900_IICSTAT_LASTBIT) |
                                (s->status & S5L8900_IICSTAT_LASTBIT);
                    s->status |= S5L8900_IICSTAT_TXRXEN;
                    break;
        }
        mode = (s->status >> 6) & 0x3;
        if (value & S5L8900_IICSTAT_TXRXEN) {
            /* IIC-bus data output enable/disable bit */
            switch(mode) {
            case SR_MODE:
                s->data = s5l8900_i2c_receive(s);
                break;
            case ST_MODE:
                s->data = s5l8900_i2c_receive(s);
                break;
            case MR_MODE:
                if (value & S5L8900_IICSTAT_START) {
                    /* START condition */
                    s->status &= ~S5L8900_IICSTAT_LASTBIT;

                    s->iicreg20 |= 0x100;
                    uint8_t addr = s5l8900_i2c_start_addr(s);
                    s->active = 1;
                    i2c_start_transfer(s->bus, addr, 1);
                    s5l8900_i2c_set_ack(s, addr);
                } else {
                    i2c_end_transfer(s->bus);
                    s->active = 0;
                    s->status |= S5L8900_IICSTAT_TXRXEN;
                }
                break;
            case MT_MODE:
                if (value & S5L8900_IICSTAT_START) {
                    /* START condition */
                    s->status &= ~S5L8900_IICSTAT_LASTBIT;
                        
                    s->iicreg20 |= 0x100;
                    uint8_t addr = s5l8900_i2c_start_addr(s);
                    s->active = 1;
                    i2c_start_transfer(s->bus, addr, 0);
                    s5l8900_i2c_set_ack(s, addr);
                } else {
                    i2c_end_transfer(s->bus);
                    s->active = 0;
                    s->status |= S5L8900_IICSTAT_TXRXEN;
                }
                break;
            default:
                break;
            }
        }
        s5l8900_i2c_update(s);
        break;

    case I2CADD:
        s->address = value & 0xff;
        break;

    case I2CDS:
        s5l8900_i2c_send(s, value & 0xff);
        break;

    case I2CLC:
        s->line_ctrl = value & 0xff;
        break;

    case IICREG20:
        //s->iicreg20 &= ~value;
        break;
    default:
        break;
        //hw_error("s5l8900.i2c: bad write offset 0x" TARGET_FMT_plx "\n", offset);
        //fprintf(stderr, "%s: bad write offset 0x%08x\n", __func__, offset);
    }
}

static const MemoryRegionOps ipod_touch_i2c_ops = {
    .read = ipod_touch_i2c_read,
    .write = ipod_touch_i2c_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static int i2c_index = 0;

static void ipod_touch_i2c_init(Object *obj)
{
    DeviceState *dev = DEVICE(obj);
    IPodTouchI2CState *s = IPOD_TOUCH_I2C(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_i2c_ops, s, TYPE_IPOD_TOUCH_I2C, 0x100);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);

    char bus_name[5];
    sprintf(bus_name, "i2c%d", i2c_index);

    s->bus = i2c_init_bus(dev, bus_name);
    i2c_index += 1;
}

static void ipod_touch_i2c_reset(DeviceState *d)
{
    
}

/* The I2CBus itself is machine wiring; its slaves migrate their own state
 * through their own VMStateDescriptions. */
static const VMStateDescription vmstate_ipod_touch_i2c = {
    .name = "ipod_touch_i2c",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(control, IPodTouchI2CState),
        VMSTATE_UINT8(status, IPodTouchI2CState),
        VMSTATE_UINT8(address, IPodTouchI2CState),
        VMSTATE_UINT8(datashift, IPodTouchI2CState),
        VMSTATE_UINT8(line_ctrl, IPodTouchI2CState),
        VMSTATE_UINT32(iicreg20, IPodTouchI2CState),
        VMSTATE_UINT8(active, IPodTouchI2CState),
        VMSTATE_UINT8(ibmr, IPodTouchI2CState),
        VMSTATE_UINT8(data, IPodTouchI2CState),
        VMSTATE_UINT8(cur_addr, IPodTouchI2CState),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_i2c_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    device_class_set_legacy_reset(dc, ipod_touch_i2c_reset);
    dc->vmsd = &vmstate_ipod_touch_i2c;
}

static const TypeInfo ipod_touch_i2c_type_info = {
    .name = TYPE_IPOD_TOUCH_I2C,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchI2CState),
    .instance_init = ipod_touch_i2c_init,
    .class_init = ipod_touch_i2c_class_init,
};

static void ipod_touch_i2c_register_types(void)
{
    type_register_static(&ipod_touch_i2c_type_info);
}

type_init(ipod_touch_i2c_register_types)
