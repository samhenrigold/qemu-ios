#!/usr/bin/env python3
"""Direct-kernel boot bundles for the S5L8920/8922 boards (-M n18, -M n88): ipad1_kboot.py's stand-in for
iBoot, with the board's values where the iPad's differ.

    s5l8920_kboot.py BOARD [--identity FILE] [--ramdisk DMG] [--nor] DEC_DIR OUT [BOOT_ARGS]

BOARD is n18 or n88. Everything else (bundle format, memory layout, identity, RAM-disk mode) is
ipad1_kboot's; this only swaps the panel geometry and rewrites the chosen/arm-io values iBoot would have
set differently on these boards. The panel is portrait (320x480), so the boot logo is drawn upright.
--nor grafts the N88's SPI NOR node onto spi0 (pair it with the machine's nor-rw=), for a NAND-booting board.
"""
import os, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import ipad1_kboot as k

# board-id / chip-id as these boards' iBoot writes them (BDID: N18AP 0x02, N88AP 0x00).
BOARDS = {
    "n18": {"platform-name": "s5l8922x", "chip-id": 0x8922, "board-id": 0x02},
    "n88": {"platform-name": "s5l8920x", "chip-id": 0x8920, "board-id": 0x00},
}
FB_WIDTH, FB_HEIGHT = 320, 480

# The N88's own spi0/nor-flash subtree (8C148a DT, phandles dropped): 1 MiB SPI NOR with diagnostic data,
# NVRAM, the raw device and effaceable storage. Grafted onto boards that boot from NAND (the N18),
# whose NVRAM and effaceable storage otherwise sit behind the NAND boot partition table (README debt 1).
NOR_FLASH = ("nor-flash", [("compatible", b"nor-flash,spi\0"), ("#address-cells", 1), ("device_type", b"nor-flash\0"),
                           ("#size-cells", 1), ("ranges", (0, 0, 0x100000)), ("reg", (0, 0x53, 0x08010000, 0, 0, 0, 0, 0))], [
    ("diagnostic-data", [("compatible", b"diagnostic-data,format1\0"), ("device_type", b"diagnostic-data\0"),
                         ("reg", (0x6000, 0x2000, 0x4000, 0x2000))], []),
    ("nvram", [("compatible", b"nvram,chrp\0"), ("device_type", b"nvram\0"), ("reg", (0xfc000, 0x2000, 0xfe000, 0x2000))], []),
    ("raw-device", [("compatible", b"raw-device,non-nvram\0"), ("device_type", b"raw-device\0"),
                    ("reg", (0x8000, 0xf2000, 0, 0x1000))], []),
    ("effaceable", [("compatible", b"effaceable,nor\0"), ("device_type", b"effaceable\0"),
                    ("reg", (0xfa000, 0x1000, 0xfb000, 0x1000))], []),
])


def dt_node(name, props, children):
    """A flattened DT node: name first, then props (bytes, int or tuple of u32), then children."""
    def prop(key, value):
        if isinstance(value, int):
            value = struct.pack("<I", value)
        elif isinstance(value, tuple):
            value = struct.pack(f"<{len(value)}I", *value)
        return key.encode().ljust(32, b"\0") + struct.pack("<I", len(value)) + value.ljust((len(value) + 3) & ~3, b"\0")
    props = [("name", name.encode() + b"\0")] + props
    return (struct.pack("<II", len(props), len(children)) + b"".join(prop(*p) for p in props)
            + b"".join(dt_node(*c) for c in children))


def graft(blob, parent, node):
    """blob with node added as parent's first child."""
    dt = k.DeviceTree(blob)
    at, props_end = dt.nodes[parent]
    buf = bytearray(blob)
    buf[props_end:props_end] = dt_node(*node)
    struct.pack_into("<I", buf, at + 4, struct.unpack_from("<I", buf, at + 4)[0] + 1)
    k.DeviceTree(bytes(buf))     # still parses end to end
    return bytes(buf)


def logo_segments(blob, fb_pa):
    """An iBootIm logo centred upright on a black portrait framebuffer (grey + inverted alpha, as k's)."""
    assert blob[:8] == b"iBootIm\0" and blob[12:16] == b"sszl" and blob[16:20] == b"yerg"
    w, h = struct.unpack_from("<HH", blob, 20)
    px = k.lzss(blob[0x40:])
    stride, x0, y0 = FB_WIDTH * 4, (FB_WIDTH - w) // 2, (FB_HEIGHT - h) // 2
    rows = bytearray(stride * h)
    for y in range(h):
        for x in range(w):
            grey, clear = px[(y * w + x) * 2], px[(y * w + x) * 2 + 1]
            struct.pack_into("<I", rows, y * stride + (x0 + x) * 4, 0xFF000000 | grey * (255 - clear) // 255 * 0x010101)
    return [(fb_pa, stride * FB_HEIGHT, None), (fb_pa + y0 * stride, len(rows), bytes(rows))]


def build(board, dec_dir, boot_args, ident, ramdisk=None, nor=False):
    k.FB_WIDTH, k.FB_HEIGHT = FB_WIDTH, FB_HEIGHT
    # ponytail: the iPad's clock table cut to the 32 slots these DTs reserve; the 8920 slot meanings
    # are unchecked (iBoot's clock_get_frequency indices). Fix when a driver reads a wrong rate.
    k.CLOCKS = k.CLOCKS[:32]
    dt_blob = open(os.path.join(dec_dir, "DeviceTree.bin"), "rb").read()
    if nor:
        dt_blob = graft(dt_blob, "arm-io/spi0", NOR_FLASH)
    image, load_pa, entry_pa, args_pa = k.build(os.path.join(dec_dir, "kernelcache.mach"), dt_blob, boot_args, ident,
                                                k.iboot_version(dec_dir), ramdisk)
    image = bytearray(image)
    vbase = struct.unpack_from("<I", image, args_pa - load_pa + 4)[0]
    dt_va, dt_len = struct.unpack_from("<II", image, args_pa - load_pa + 0x30)
    off = dt_va - vbase
    dt = k.DeviceTree(bytes(image[off:off + dt_len]))
    b = BOARDS[board]
    dt.set("", "platform-name", b["platform-name"])
    for key in ("chip-id", "board-id"):
        dt.set("chosen", key, b[key])
    dt.set("chosen", "display-rotation", 0)
    # ponytail: no NAND boot partition. These boards boot from NAND: IOFlashPartitionScheme claims the
    # flash for a boot-block partition table (LLB, iBoot, NVRAM) that only a restore writes, and without
    # one no FTL attaches. Hiding boot-from-nand makes the FTL take the whole device as on the iPad
    # (block offset 1). Replace with a restored NAND partition table.
    disk = "arm-io/flash-controller0/disk"
    if "boot-from-nand" in dt.props.get(disk, {}):
        dt.rename(disk, "boot-from-nand", "boot-from-nand-off")
    assert len(dt.buf) == dt_len
    image[off:off + dt_len] = dt.buf
    return bytes(image), load_pa, entry_pa, args_pa


def main(board, dec_dir, out, boot_args=k.DEFAULT_BOOT_ARGS, identity=k.IDENTITY_FILE, ramdisk=None, nor=False):
    image, load_pa, entry_pa, args_pa = build(board, dec_dir, boot_args, k.load_identity(identity),
                                              open(ramdisk, "rb").read() if ramdisk else None, nor)
    logo = os.path.join(dec_dir, "AppleLogo.bin")
    segments = logo_segments(open(logo, "rb").read(), k.VRAM_PA) if os.path.exists(logo) else []
    with open(out, "wb") as f:
        f.write(image + k.pack_segments(segments) + k.TRAILER.pack(b"K48KBOOT", load_pa, entry_pa, args_pa, len(image)))
    print(f"{board}: load {load_pa:#x}+{len(image):#x} entry {entry_pa:#x} r0 {args_pa:#x}")


if __name__ == "__main__":
    argv = sys.argv[1:]
    if not argv or argv[0] not in BOARDS:
        sys.exit(__doc__)
    board, argv, identity, ramdisk, nor = argv[0], argv[1:], k.IDENTITY_FILE, None, False
    while argv[:1] in (["--identity"], ["--ramdisk"], ["--nor"]):
        if argv[0] == "--nor":
            nor, argv = True, argv[1:]
            continue
        if len(argv) < 2:
            sys.exit(__doc__)
        if argv[0] == "--identity":
            identity = argv[1]
        else:
            ramdisk = argv[1]
        argv = argv[2:]
    if len(argv) not in (2, 3):
        sys.exit(__doc__)
    main(board, *argv, identity=identity, ramdisk=ramdisk, nor=nor)
