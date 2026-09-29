/*
 * iBoot literal discovery shared by the boards that stage a decrypted iBoot
 * themselves. See include/hw/arm/it_iboot.h.
 */
#include "qemu/osdep.h"
#include "hw/arm/it_iboot.h"

static uint16_t iboot_u16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

static uint32_t iboot_u32(const uint8_t *p)
{
    return iboot_u16(p) | (uint32_t)iboot_u16(p + 2) << 16;
}

/* Does any Thumb `ldr rN, [pc, #imm]` before `word` load that literal? */
static bool iboot_literal_loaded(const uint8_t *image, size_t word)
{
    for (size_t i = 0; i + 2 <= word; i += 2) {
        uint16_t hw = iboot_u16(image + i);
        if ((hw & 0xf800) == 0x4800 &&
            ((i + 4) & ~(size_t)3) + (hw & 0xff) * 4 == word) {
            return true;
        }
    }
    return false;
}

uint32_t it_iboot_find_boot_args_literal(const uint8_t *image, size_t size,
                                         uint32_t base)
{
    /*
     * Release iBoot ignores NVRAM boot-args: it hands XNU an empty string on
     * a normal boot and the restore string in restore mode, each through a
     * literal, and every iPod touch 2G iBoot (385.22 .. 931.71.16) keeps the
     * normal-boot literal in the word before the restore one. So: the restore
     * string, its one literal, the word before it -- provided Thumb code
     * really loads that word and it points at an empty string in the image.
     */
    static const char restore[] = "rd=md0 nand-enable-reformat=1 -progress";
    size_t rs = 0, found = 0;

    if (!image || size < sizeof(restore) + 8 || size > UINT32_MAX - base) {
        return 0;
    }
    for (size_t i = 0; i + sizeof(restore) <= size; i++) {
        if (!memcmp(image + i, restore, sizeof(restore))) {
            if (rs) {
                return 0;
            }
            rs = i;
        }
    }
    if (!rs) {
        return 0;
    }
    for (size_t i = 4; i + 4 <= size; i += 4) {
        if (iboot_u32(image + i) != base + rs) {
            continue;
        }
        if (found) {
            return 0;
        }
        found = i - 4;
    }
    if (!found) {
        return 0;
    }
    uint32_t string = iboot_u32(image + found);
    if (!iboot_literal_loaded(image, found) || string < base ||
        string - base >= size || image[string - base] != 0) {
        return 0;
    }
    return found;
}

/*
 * iBoot-204 (the S5L8900's, iPhone OS 1.x) has no epoch helper: miu_init
 * compares the byte inline, `ldr rN, [rN]; lsrs rN, rN, #24; cmp rN, #M;
 * beq ok` on POWER_ID, and the fall-through loads "miu_init: Epoch
 * Mismatch" for panic() (M is 2 in 3A101a, 3 in 4B1).
 */
static uint32_t iboot_inline_epoch(const uint8_t *image, size_t size)
{
    static const char panic_text[] = "Epoch Mismatch";
    uint32_t found = 0;

    for (size_t i = 0; i + 14 <= size; i += 2) {
        uint16_t ld = iboot_u16(image + i), sh = iboot_u16(image + i + 2);
        uint16_t cmp = iboot_u16(image + i + 4), br = iboot_u16(image + i + 6);
        unsigned r = ld & 7;
        if ((ld & 0xffc0) != 0x6800 || ((ld >> 3) & 7) != r ||   /* ldr rN, [rN] */
            sh != (0x0e00 | r << 3 | r) ||                       /* lsrs rN, rN, #24 */
            (cmp & 0xff00) != (0x2800 | r << 8) ||               /* cmp rN, #M */
            (br & 0xff00) != 0xd000) {                           /* beq */
            continue;
        }
        bool panics = false;
        for (size_t j = i + 8; j < i + 14; j += 2) {              /* the panic's ldr r0/r1, =string */
            uint16_t lit = iboot_u16(image + j);
            size_t at = ((j + 4) & ~(size_t)3) + (lit & 0xff) * 4;
            if ((lit & 0xf800) != 0x4800 || at + 4 > size) {
                continue;
            }
            uint32_t s = iboot_u32(image + at) & 0xfffff;         /* image offset: iBoot runs at a 1 MiB boundary */
            panics |= s < size && memmem(image + s, size - s < 64 ? size - s : 64,
                                         panic_text, sizeof(panic_text) - 1);
        }
        if (!panics) {
            continue;
        }
        if (found) {
            return 0;
        }
        found = cmp & 0xff;
    }
    return found;
}

uint32_t it_iboot_find_epoch(const uint8_t *image, size_t size)
{
    /*
     * iBoot's security-epoch helper: `bl chipid_epoch_field; cmp r0, #N;
     * bhi/bne +2; movs r0, #M` -- the fused field floored at this build's
     * epoch M (1 for 385.22, 2 for 385.49, 3 for 596, 4 for 636+). The
     * field accessor is byte-identical in every image:
     * ldr r0, [pc, #8]; ldr r0, [r0]; lsls r0, #21; lsrs r0, #25; bx lr;
     * literal 0x3d100008 (CHIPID_INFO).
     */
    static const uint8_t accessor[] = { 0x02, 0x48, 0x00, 0x68, 0x40, 0x05,
                                        0x40, 0x0e, 0x70, 0x47 };
    size_t acc = 0;
    uint32_t found = 0;

    if (!image || size < 16) {
        return 0;
    }
    if ((found = iboot_inline_epoch(image, size))) {
        return found;
    }
    for (size_t i = 0; i + 16 <= size; i += 2) {
        if (!memcmp(image + i, accessor, sizeof(accessor)) &&
            iboot_u32(image + i + 12) == 0x3d100008) {
            if (acc) {
                return 0;
            }
            acc = i;
        }
    }
    if (!acc) {
        return 0;
    }
    for (size_t j = 0; j + 12 <= size; j += 2) {
        uint16_t h1 = iboot_u16(image + j), h2 = iboot_u16(image + j + 2);
        int32_t off;
        if ((h1 & 0xf800) != 0xf000 || (h2 & 0xf800) != 0xf800) {
            continue;                                   /* not Thumb bl */
        }
        off = ((h1 & 0x7ff) << 12) | ((h2 & 0x7ff) << 1);
        if (off & 0x400000) {
            off -= 0x800000;
        }
        if ((int64_t)j + 4 + off != (int64_t)acc) {
            continue;
        }
        uint16_t cmp = iboot_u16(image + j + 4), br = iboot_u16(image + j + 6);
        uint16_t mov = iboot_u16(image + j + 8);
        if ((cmp & 0xff00) != 0x2800 ||                  /* cmp r0, #N */
            ((br & 0xff00) != 0xd800 && (br & 0xff00) != 0xd100) ||
            (mov & 0xff00) != 0x2000) {                  /* movs r0, #M */
            continue;
        }
        if (found) {
            return 0;
        }
        found = mov & 0xff;
    }
    return found;
}

/* Thumb-2 BL at `p`: the target's offset from p + 4, or false if not a BL. */
static bool iboot_t2_bl(const uint8_t *p, int32_t *off)
{
    uint16_t h1 = iboot_u16(p), h2 = iboot_u16(p + 2);
    uint32_t s, i1, i2;

    if ((h1 & 0xf800) != 0xf000 || (h2 & 0xd000) != 0xd000) {
        return false;
    }
    s = (h1 >> 10) & 1;
    i1 = !(((h2 >> 13) & 1) ^ s);
    i2 = !(((h2 >> 11) & 1) ^ s);
    *off = (int32_t)((i1 << 23) | (i2 << 22) | ((h1 & 0x3ff) << 12) |
                     ((h2 & 0x7ff) << 1)) - (int32_t)(s << 24);
    return true;
}

uint32_t it_iboot_find_miu_epoch(const uint8_t *image, size_t size,
                                 uint32_t fuse)
{
    uint32_t found = 0;

    if (!image || size < 32) {
        return 0;
    }
    for (size_t i = 4; i + 4 <= size; i += 2) {
        int32_t off;
        size_t f;

        /* cmp.w r0, rN, lsr #24, right after the bl epoch() */
        if (iboot_u16(image + i) != 0xebb0 ||
            (iboot_u16(image + i + 2) & 0xfff0) != 0x6f10 ||
            !iboot_t2_bl(image + i - 4, &off) ||
            (int64_t)i + off < 0 || (size_t)((int64_t)i + off) + 18 > size) {
            continue;
        }
        f = i + off;
        /* push {r7, lr}; add r7, sp, #0 | mov r7, sp */
        if (iboot_u16(image + f) != 0xb580 ||
            (iboot_u16(image + f + 2) != 0xaf00 &&
             iboot_u16(image + f + 2) != 0x466f)) {
            continue;
        }
        uint16_t cmp = iboot_u16(image + f + 8), it = iboot_u16(image + f + 10);
        uint16_t mov = iboot_u16(image + f + 12);
        if (!iboot_t2_bl(image + f + 4, &off) || (cmp & 0xff00) != 0x2800 ||
            (it & 0xff0f) != 0xbf08 || (mov & 0xff00) != 0x2000) {
            continue;
        }
        uint32_t n = cmp & 0xff, m = mov & 0xff, taken;
        switch ((it >> 4) & 0xf) {
        case 0x0: taken = fuse == n; break;     /* eq: 817, 931 */
        case 0x3: taken = fuse < n; break;      /* lo: 1219 */
        case 0x9: taken = fuse <= n; break;     /* ls: 1072 */
        default: continue;
        }
        if (found) {
            return 0;
        }
        found = taken ? m : fuse;
    }
    return found;
}

uint32_t it_iboot_find_command_line(const uint8_t *image, size_t size,
                                    uint32_t base)
{
    static const char format[] = "gBootArgs.commandLine = [%s]\n";
    uint32_t found = 0;
    if (!image || size < sizeof(format) || size > UINT32_MAX - base || (base & 3)) {
        return 0;
    }
    for (size_t i = 0; i + 10 <= size; i += 2) {
        /* ldr r4, literal; ldr r0, literal; adds r1, r4, #0; bl printf */
        uint16_t buffer_load = iboot_u16(image + i);
        uint16_t format_load = iboot_u16(image + i + 2);
        if ((buffer_load & 0xff00) != 0x4c00 ||
            (format_load & 0xff00) != 0x4800 ||
            iboot_u16(image + i + 4) != 0x1c21 ||
            (iboot_u16(image + i + 6) & 0xf800) != 0xf000 ||
            (iboot_u16(image + i + 8) & 0xf800) != 0xf800) {
            continue;
        }
        size_t bp = ((i + 4) & ~(size_t)3) + (buffer_load & 255) * 4;
        size_t fp = ((i + 6) & ~(size_t)3) + (format_load & 255) * 4;
        if (bp > size - 4 || fp > size - 4) {
            continue;
        }
        uint32_t buffer = iboot_u32(image + bp);
        uint32_t text = iboot_u32(image + fp);
        if (text < base || text - base > size - sizeof(format) ||
            memcmp(image + (text - base), format, sizeof(format)) ||
            size < 256 || buffer < base || (buffer & 3) ||
            buffer - base > size - 256) {
            continue;
        }
        if (found) {
            return 0;
        }
        found = buffer;
    }
    return found;
}

#ifndef IT_IBOOT_HOST_TEST
uint32_t it_iboot_epoch(AddressSpace *as, uint32_t base, size_t image_size)
{
    uint32_t epoch = 0;
    if (!image_size || image_size > 0x100000) {
        return 0;
    }
    g_autofree uint8_t *image = g_try_malloc(image_size);
    if (image) {
        address_space_read(as, base, MEMTXATTRS_UNSPECIFIED, image, image_size);
        epoch = it_iboot_find_epoch(image, image_size);
    }
    return epoch;
}

uint32_t it_iboot_inject_boot_args(AddressSpace *as, uint32_t base,
                                   size_t image_size, const char *args,
                                   hwaddr staging)
{
    uint8_t literal[4], command[256] = { 0 };
    uint32_t found;

    if (!args || !image_size || image_size > 0x100000) {
        return 0;
    }
    g_autofree uint8_t *image = g_try_malloc(image_size);
    if (!image) {
        return 0;
    }
    address_space_read(as, base, MEMTXATTRS_UNSPECIFIED, image, image_size);
    found = it_iboot_find_boot_args_literal(image, image_size, base);
    if (!found) {
        return 0;
    }
    g_strlcpy((char *)command, args, sizeof(command));
    address_space_write(as, staging, MEMTXATTRS_UNSPECIFIED, command,
                        sizeof(command));
    stl_le_p(literal, staging);
    address_space_write(as, base + found, MEMTXATTRS_UNSPECIFIED, literal,
                        sizeof(literal));
    return base + found;
}
#endif
