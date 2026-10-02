#include "hw/arm/ipod_touch_chipid.h"
#include "qemu/log.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"

static uint64_t ipod_touch_chipid_read(void *opaque, hwaddr addr, unsigned size)
{
    IPodTouchChipIDState *s = opaque;

    switch (addr) {
        case CHIPID_UNKNOWN1:
            return s->word1;
        case CHIPID_INFO:
            return s->word2;
        case CHIPID_UNKNOWN2:
            return s->word3;
        case CHIPID_UNKNOWN3:
            return s->word4;
        default:
            /*
             * Offset 0 is inside the 0x14 region but has no named register, and
             * a guest read of it used to hw_error() the whole process. Read as
             * 0, like every other unhandled ChipID offset.
             */
            qemu_log_mask(LOG_UNIMP,
                          "%s: read from unknown chip ID register 0x%08x\n",
                          __func__, (uint32_t)addr);
    }

    return 0;
}

/* Stock S5L8720 ROM 240.4: production is +4 bit5; secure is +8 bit1
 * OR production. CPFM packs secure into bit0, production into bit1.
 * +8 bits3:2 are SDOM and bit0 is the oscillator input: neither changes.
 * Apply before realize, never on a guest read or during reset.
 */
void ipod_touch_chipid_set_n72_profile(IPodTouchChipIDState *s,
                                      N72SecurityProfile profile)
{
    switch (profile) {
    case N72_SECURITY_RETAIL:
        /* Preserve production defaults and explicit physical fuse inputs. */
        break;
    case N72_SECURITY_SECURE_DEVELOPMENT:
        s->word1 &= ~(1u << 5);
        s->word2 |= 1u << 1;
        break;
    case N72_SECURITY_INSECURE_DEVELOPMENT:
        s->word1 &= ~(1u << 5);
        s->word2 &= ~(1u << 1);
        break;
    default:
        g_assert_not_reached();
    }
}

static void ipod_touch_chipid_write(void *opaque, hwaddr addr, uint64_t val,
                                    unsigned size)
{
    /* ChipID is read-only fuses; there is no .write path on hardware. Swallow
     * stores (the ops table had no .write at all, so any store dispatched
     * through a NULL pointer). */
    qemu_log_mask(LOG_GUEST_ERROR,
                  "%s: write 0x%08x to read-only chip ID register 0x%08x\n",
                  __func__, (uint32_t)val, (uint32_t)addr);
}

static const MemoryRegionOps ipod_touch_chipid_ops = {
    .read = ipod_touch_chipid_read,
    .write = ipod_touch_chipid_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
};

static void ipod_touch_chipid_init(Object *obj)
{
    IPodTouchChipIDState *s = IPOD_TOUCH_CHIPID(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    memory_region_init_io(&s->iomem, obj, &ipod_touch_chipid_ops, s, TYPE_IPOD_TOUCH_CHIPID, 0x14);
    sysbus_init_mmio(sbd, &s->iomem);
}

/* The fuse words as properties so one model serves both SoCs: the S5L8720
 * reads production/security fuses and the chip id at +4/+8; the S5L8900's
 * iBoot only reads the revision at +4 (0x02 << 24). */
static const Property ipod_touch_chipid_properties[] = {
    DEFINE_PROP_UINT32("word1", IPodTouchChipIDState, word1, 1u << 5),
    DEFINE_PROP_UINT32("word2", IPodTouchChipIDState, word2, (0x8720u << 16) | (1u << 2)),
    /* Stock S5L8720 ROM/iBSS assembles ECID from these read-only fuse words.
     * Defaults preserve existing devices; host provisioning supplies identity.
     * These are hardware inputs, not edits to a USB descriptor or guest code. */
    DEFINE_PROP_UINT32("word3", IPodTouchChipIDState, word3, 0),
    DEFINE_PROP_UINT32("word4", IPodTouchChipIDState, word4, 0),
};

/* Fuse configuration is an input, never migrated into a different device.
 * Streams predating this section are not covered by this comparison guard.
 */
static const VMStateDescription vmstate_ipod_touch_chipid = {
    .name = TYPE_IPOD_TOUCH_CHIPID,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(word1, IPodTouchChipIDState, "ChipID production/revision fuse mismatch"),
        VMSTATE_UINT32_EQUAL(word2, IPodTouchChipIDState, "ChipID secure/domain/clock fuse mismatch"),
        VMSTATE_UINT32_EQUAL(word3, IPodTouchChipIDState, "ChipID identity fuse mismatch"),
        VMSTATE_UINT32_EQUAL(word4, IPodTouchChipIDState, "ChipID identity fuse mismatch"),
        VMSTATE_END_OF_LIST()
    }
};

static void ipod_touch_chipid_class_init(ObjectClass *klass, void *data)
{
    DEVICE_CLASS(klass)->vmsd = &vmstate_ipod_touch_chipid;
    device_class_set_props(DEVICE_CLASS(klass), ipod_touch_chipid_properties);
}

static const TypeInfo ipod_touch_chipid_type_info = {
    .name = TYPE_IPOD_TOUCH_CHIPID,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(IPodTouchChipIDState),
    .instance_init = ipod_touch_chipid_init,
    .class_init = ipod_touch_chipid_class_init,
};

static void ipod_touch_chipid_register_types(void)
{
    type_register_static(&ipod_touch_chipid_type_info);
}

type_init(ipod_touch_chipid_register_types)