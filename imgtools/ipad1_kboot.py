#!/usr/bin/env python3
"""Build a direct-kernel boot image for the ipad1 machine: what iBoot-817.29 does before it jumps to xnu.

    ipad1_kboot.py [--identity FILE] [--ramdisk DMG] DEC_DIR OUT [BOOT_ARGS]
    ipad1_kboot.py --synth-identity SEED OUT.json    a synthetic identity (see synth_identity)

DEC_DIR is ipad1_fw.py's output (kernelcache.mach, DeviceTree.bin). BOOT_ARGS defaults to DEFAULT_BOOT_ARGS.
--identity defaults to IDENTITY_FILE. With no arguments only the self-check runs.
--ramdisk DMG boots a raw-HFS RAM disk as root, the way iBoot boots a restore: the image sits in DRAM
between the kernel and the DeviceTree, chosen/memory-map gets a RAMDisk (pa, len) entry, boot-args gain
rd=md0 and chosen/root-matching is left empty (xnu reads RAMDisk only when root-matching does not match).
The DeviceTree's own secure-root-prefix ('md' on 4.x) is untouched, so the md0 root is a SecureRoot. The kernel is stock: the
USB Ethernet link is raised by the baked it_ethlink helper (contrib/it-ethlink).

OUT format (all little-endian): a flat image of physical memory, extra segments, then a 24-byte trailer.

    [0, image_len)      bytes to place at physical load_pa (0x40000000, DRAM base)
    segments            char magic[8] = "K48SEG\0\0"; u32 pa, len, flags; then len bytes unless
                        flags bit 0 (zero-fill). Used for the boot-logo framebuffer; loaders that
                        predate them skip to the trailer and just boot without the logo.
    trailer:  char magic[8] = "K48KBOOT"; u32 load_pa; u32 entry_pa; u32 bootargs_pa; u32 image_len

Boot logo: iBoot draws the IPSW's AppleLogo (an "iBootIm" image) centred on a black framebuffer and the
kernel keeps it on screen until SpringBoard draws, unless boot-args carry -v. DEC_DIR/AppleLogo.bin, when
present, is drawn the same way into vram, turned to the panel's orientation.

Loader contract: 256 MiB DRAM at 0x40000000. Copy the image (or the whole file; the trailer then lands in
padding nothing uses) to load_pa, then start the CPU at entry_pa in ARM state, SVC mode, IRQ/FIQ masked,
MMU and caches off, r0 = bootargs_pa, other registers 0. The kernel builds its first page table at
topOfKernelData (boot_args+0x10, 16 KiB aligned, just past the image) and zeroes 0x9000 bytes there.

Physical layout, mirroring iBoot's allocator (kernel VA base = PA 0x40000000; the base is the kernel's own
link base, 0xC0000000 on 3.x and 0x80000000 on 4.x):
    kernel segments   PA = vmaddr - 0x80000000 (filesize copied, the rest of vmsize zeroed)
    RAMDisk           (--ramdisk only) next page after the highest segment end
    DeviceTree        next page after that
    BootArgs          next page after the DT (one page; the struct is 0x138 bytes)
    topOfKernelData   end of BootArgs rounded up to 16 KiB
    memSize           0x0F700000: DRAM less pram (0x4000) and vram (0x8FC000), as iBoot computes it
    vram              0x4F700000 + 0x8FC000 (iBoot writes the 0x5F700000 alias; same RAM through the mirror)
    pram              0x4FFFC000 + 0x4000   (iBoot writes 0x5FFFC000; ditto)
"""
import hashlib, json, os, struct, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import Macho

PHYS_BASE, DRAM_SIZE = 0x40000000, 0x10000000
PRAM_SIZE, VRAM_SIZE = 0x4000, 0x900000 - 0x4000
MEM_SIZE = DRAM_SIZE - PRAM_SIZE - VRAM_SIZE
VRAM_PA, PRAM_PA = PHYS_BASE + MEM_SIZE, PHYS_BASE + DRAM_SIZE - PRAM_SIZE


def layout(board):
    """(memSize, vram PA, pram PA) for the board's DRAM: vram and pram at its top, as iBoot puts them."""
    dram = board.get("dram", DRAM_SIZE)
    mem = dram - PRAM_SIZE - VRAM_SIZE
    return mem, PHYS_BASE + mem, PHYS_BASE + dram - PRAM_SIZE
FB_DEPTH = 32
# What differs per A4 board (the machine's A4Board in hw/arm/ipad1.c), keyed by the DT's own
# compatible ("K48AP" -> k48), so the DT says which board it is.
#   machine       the QEMU machine (-M) that runs it
#   fb            the panel's scan-out size (K48 is a landscape panel under a portrait UI)
#   rotation      chosen/display-rotation, the panel's turn against the portrait UI (see fill_dt)
#   board-id      chosen/board-id, what iBoot reads off the board straps
#   model         model-number of the modelled storage size (identity default)
#   radio         the board has a baseband: leave its DT node for the machine (-M ...,baseband=)
#   dram          DRAM bytes when not K48's 256 MiB (memSize, vram/pram at its top)
#   scale         chosen/display-scale, points to pixels (2 on Retina panels)
# Boards without the SPI NOR (nvram, effaceable in NAND) get K48's grafted on (graft_nor).
BOARDS = {
    "k48": {"machine": "ipad1", "fb": (1024, 768), "rotation": 270, "scale": 1, "board-id": 0x02, "model": "MB292"},
    "n81": {"machine": "iPod-Touch-4G", "fb": (640, 960), "rotation": 0, "scale": 2, "board-id": 0x08,
            "model": "MC540"},
    "n90": {"machine": "iPhone-4", "fb": (640, 960), "rotation": 0, "scale": 2, "board-id": 0x00,
            "model": "MC603", "dram": 0x20000000, "radio": True},
}
FB_WIDTH, FB_HEIGHT = BOARDS["k48"]["fb"]
# serial bit 0 moves the console to UART0 (arm_init c005d5fe); debug=0x8 is DB_KPRT, which PE_init_kprintf
# (c01d1dce) needs before kprintf reaches the UART. No rd=: root-matching below names partition 1.
# The AMFI pair lets the ldid-signed guest tools run on a stock kernel (AMFI::start honours them because
# kboot forces debug-enabled): it_pbd (pasteboard, docs/ipad1/guest-services.md) and the GL front end
# (contrib/gles-public). Apple's own binaries are unaffected.
# No -v: like a stock boot the screen shows iBoot's Apple logo, not the text console; serial=3 still
# sends the kernel log to UART0.
# enable-hsic=1: 4.x's AppleS5L8930XUSBArbitrator::handleStart (8C148 0x80525788) publishes the host nubs
# for the DT's hsic-enabled (below) only when this boot-arg is 1, so without it no USB keyboard; 3.x reads
# only the property and ignores the argument.
DEFAULT_BOOT_ARGS = "serial=3 debug=0x8 amfi_allow_any_signature=1 cs_enforcement_disable=1 enable-hsic=1"
# chosen/firmware-version is the iBoot that booted the kernel: the IPSW's own (7B367 817.28, 7B500 817.29).
IBOOT_VERSION = "iBoot-817.29"


def iboot_version(dec_dir):
    """The "iBoot-N.N" tag out of DEC_DIR/iBoot.bin (decrypted), else IBOOT_VERSION."""
    import re
    path = os.path.join(dec_dir, "iBoot.bin")
    m = re.search(rb"iBoot-\d+(?:\.\d+)*", open(path, "rb").read()) if os.path.exists(path) else None
    return m[0].decode() if m else IBOOT_VERSION


TRAILER = struct.Struct("<8sIIII")
SEGMENT = struct.Struct("<8sIII")


def lzss(src):
    """Apple's LZSS (4 KiB window, 18-byte matches), as iBootIm and kernelcaches use."""
    window, pos, out, i, flags = bytearray(4096), 4096 - 18, bytearray(), 0, 0
    while i < len(src):
        flags >>= 1
        if not flags & 0x100:
            flags, i = src[i] | 0xFF00, i + 1
            if i >= len(src):
                break
        if flags & 1:
            at, n, literal, i = 0, 1, src[i], i + 1
        elif i + 1 < len(src):
            at, n, literal, i = src[i] | (src[i + 1] & 0xF0) << 4, (src[i + 1] & 0xF) + 3, None, i + 2
        else:
            break
        for k in range(n):       # byte by byte: a match may overlap what it is writing
            c = literal if literal is not None else window[(at + k) & 0xFFF]
            out.append(c)
            window[pos], pos = c, (pos + 1) & 0xFFF
    return bytes(out)


def logo_segments(blob, fb_pa, board=BOARDS["k48"]):
    """Framebuffer segments that put an iBootIm logo where iBoot puts it: centred on black.

    iBootIm: "iBootIm\0", adler32, "lzss", format tag (only "grey" here: grey + inverted alpha,
    composited over black), u16 width, height; LZSS data at 0x40. On a landscape panel (rotation 270) the
    portrait UI arrives turned a quarter counter-clockwise into it (its top along the panel's left edge; the
    app turns the panel a quarter clockwise to stand it up), so the logo is turned the same way."""
    assert blob[:8] == b"iBootIm\0" and blob[12:16] == b"sszl", "not an LZSS iBootIm"
    assert blob[16:20] == b"yerg", "only the grey iBootIm format is handled"
    w, h = struct.unpack_from("<HH", blob, 20)
    px = lzss(blob[0x40:])
    assert len(px) >= w * h * 2, "short iBootIm"
    fbw, fbh = board["fb"]
    turn = board["rotation"] == 270
    lw, lh = (h, w) if turn else (w, h)      # the logo as it lands on the panel
    x0, y0, stride = (fbw - lw) // 2, (fbh - lh) // 2, fbw * 4
    rows = bytearray(stride * lh)
    for ly in range(h):
        for lx in range(w):
            grey, clear = px[(ly * w + lx) * 2], px[(ly * w + lx) * 2 + 1]
            v = grey * (255 - clear) // 255
            at = (w - 1 - lx) * stride + (x0 + ly) * 4 if turn else ly * stride + (x0 + lx) * 4
            struct.pack_into("<I", rows, at, 0xFF000000 | v * 0x010101)
    return [(fb_pa, stride * fbh, None), (fb_pa + y0 * stride, len(rows), bytes(rows))]


# ponytail: clocks are guesses (timebase = the kernel's own 24 MHz default); replace with HW-2's real
# IODeviceTree values. clock-frequencies slots follow iBoot's clock_get_frequency (5ff10f80) indices.
# Measured on a real iPad 1 running 7B500 (sysctl hw.*, 2026-09-26): iBoot leaves cpu and
# memory frequency at 0; bus and peripheral are 100 MHz; fixed and timebase 24 MHz.
CPU_HZ, MEM_HZ, BUS_HZ, PERIPH_HZ, FIXED_HZ, TIMEBASE_HZ, USBPHY_HZ = (
    0, 0, 100_000_000, 100_000_000, 24_000_000, 24_000_000, 24_000_000)
# NAND geometry iBoot would have probed. The captured 16 GB iPad has eight Hynix 0xB614D5AD dies
# (4 KiB pages, 128 B spare, 0x1000 blocks per CE, 2 buses x 4 CE): its /dev/rdisk0 sector count,
# 3,925,449 x 4 KiB, matches the 7B500 YAFTL_Init formulas for that part only. bbt-format 3 is the
# Hynix row of iBoot-817.29's chip table. Must agree with the IOP model's nand-id/nand-ce-mask.
NAND = {"#ce": 8, "#die-ce": 1, "#ce-blocks": 0x1000, "#block-pages": 128, "#page-bytes": 4096,
        "#spare-bytes": 0x80, "device-readid": 0xB614D5AD, "vendor-type": 0x100014, "#databus": 2,
        "ecc-correctable": 8, "ecc-threshold": 8, "bbt-format": 3,
        "read-cycle-ns": 25, "read-setup-ns": 10, "read-hold-ns": 10, "read-delay-ns": 20,
        "read-valid-ns": 20, "write-cycle-ns": 25, "write-hold-ns": 10,
        # iBoot-931 (4.x) DTs replace the *-ns timings with *-clks (unused by the IOP model, left 0) and add
        # the FMI meta layout, which 4.x's IOPFMI hands to the firmware in set_config (3.x's firmware
        # hard-coded it): 10 bytes of meta DMA per page, in YaFTL's 12-byte struct (4.x yaFTL panics "meta
        # struct size (12) not equal to bytes per metadata" otherwise). The PPN props (cau-bits, blocks-cau,
        # ...) stay 0: iBoot-931 fills them only when ppn-device = 1 (iBoot 0x5ff077fc), and this is raw NAND.
        "meta-per-logical-page": 12, "valid-meta-per-logical-page": 10, "logical-page-size": 4096,
        "ppn-device": 0}
# The unit's identity, from FILES/identity.json (untracked, mode 600; never commit it): what iBoot
# would put in the DT from the fuses and syscfg. lockdownd's UniqueDeviceID on a 3.2 iPad is
# SHA1(serial + Wi-Fi MAC + Bluetooth MAC), so the captured activation record only validates with the
# real unit's values. Without the file the image gets obviously synthetic ones.
#   {"serial-number": "...", "mlb-serial-number": "...", "unique-chip-id": "0x<ECID>",
#    "die-id": ["0x<word 2>", "0x<word 3>"], "wifi-mac": "aa:bb:cc:dd:ee:ff", "bt-mac": "..."}
IDENTITY_FILE = os.path.expanduser("~/Developer/qemu-ios-files/ipad1/identity.json")
PLACEHOLDER = {"serial-number": "EMU000000000", "mlb-serial-number": "EMU0000000000",
               "unique-chip-id": "0x0000000001", "die-id": ["0x0", "0x0"],
               "wifi-mac": "02:00:00:00:00:01", "bt-mac": "02:00:00:00:00:02"}
MODEL = {"model-number": "MB292", "region-info": "LL/A"}
# Wi-Fi iPad 1 model numbers by storage (the only NAND geometry modelled is 16 GB).
MODELS = {"16g": "MB292"}
SERIAL_CHARS = "0123456789ABCDEFGHJKLMNPQRSTUVWXYZ"   # no I or O, as Apple serials


def udid(ident):
    """lockdownd's UniqueDeviceID on a Wi-Fi iPad 1: SHA1(serial + Wi-Fi MAC + Bluetooth MAC), MACs lowercase
    and colon-separated (docs/research/userland-boot.md, "Activation identity")."""
    return hashlib.sha1((ident["serial-number"] + ident["wifi-mac"].lower() + ident["bt-mac"].lower()).encode()).hexdigest()


def synth_identity(seed, storage="16g", board="k48"):
    """A made-up but well-formed unit identity, a pure function of `seed`: 11-character serial, 13-character
    MLB, 40-bit ECID, two die-id words, a locally administered Wi-Fi MAC (02:...) and Bluetooth = Wi-Fi + 1."""
    h = hashlib.sha256(seed.encode()).digest()
    chars = lambda b, n: "".join(SERIAL_CHARS[x % len(SERIAL_CHARS)] for x in b[:n])
    wifi = bytes([0x02]) + h[28:32] + bytes([h[27] & 0xFE])        # even last byte: BT = +1 never carries
    bt = wifi[:5] + bytes([wifi[5] + 1])
    ident = {"serial-number": chars(h[0:], 11), "mlb-serial-number": chars(h[11:], 13),
             "unique-chip-id": "0x%010x" % (int.from_bytes(h[20:25], "big") | 1),
             "die-id": ["0x%08x" % int.from_bytes(h[25:27] + h[0:2], "big"), "0x%08x" % int.from_bytes(h[2:6], "big")],
             "wifi-mac": wifi.hex(":"), "bt-mac": bt.hex(":"),
             "model-number": MODELS[storage] if board == "k48" else BOARDS[board]["model"], "region-info": MODEL["region-info"], "seed": seed}
    # SecureROM constructs ECID from CHIPID words 2/3, and CPRV from bits
    # 10..15 of word 3. Keep revision 0x11 and make the advertised identity
    # agree with the ROM rather than choosing unrelated fuse words.
    ecid = int(ident["unique-chip-id"], 16)
    word2 = ((ecid >> 21) & 0x1fffff) | (((ecid >> 16) & 31) << 21) | (((ecid >> 2) & 63) << 26)
    word3 = (int(ident["die-id"][1], 16) & 0xffff0000) | 0x2400 | (((ecid >> 8) & 255) << 2) | (ecid & 3)
    ident["die-id"] = [f"0x{word2:08x}", f"0x{word3:08x}"]
    ident["udid"] = udid(ident)
    return ident


def load_identity(path=IDENTITY_FILE):
    """The unit identity from `path`, or PLACEHOLDER (with a warning) if it is absent."""
    if not os.path.exists(path):
        print(f"warning: {path} not found; using placeholder serial/ECID/MACs, so the captured "
              "activation record will not validate", file=sys.stderr)
        return dict(PLACEHOLDER)
    with open(path) as f:
        ident = json.load(f)
    missing = set(PLACEHOLDER) - set(ident)
    if missing:
        raise SystemExit(f"{path}: missing {sorted(missing)}")
    return ident


def identity_dt(ident):
    """(root props, chosen props, {node: local-mac-address}) for an identity dict."""
    ecid = int(ident["unique-chip-id"], 16)
    mac = lambda s: bytes.fromhex(s.replace(":", ""))
    model = {k: ident.get(k, v) for k, v in MODEL.items()}
    return ({"serial-number": ident["serial-number"], "mlb-serial-number": ident["mlb-serial-number"], **model},
            {"unique-chip-id": (ecid & 0xFFFFFFFF, ecid >> 32),
             "die-id": tuple(int(w, 16) for w in ident["die-id"])},
            {"arm-io/sdio": mac(ident["wifi-mac"]), "bluetooth": mac(ident["bt-mac"])})


CLOCKS = [PERIPH_HZ] * 55
for idx, hz in {0: TIMEBASE_HZ, 5: CPU_HZ, 6: PERIPH_HZ, 27: MEM_HZ, 32: BUS_HZ, 33: FIXED_HZ}.items():
    CLOCKS[idx] = hz

ROOT_MATCHING = ("<dict><key>IOProviderClass</key><string>IOMedia</string><key>IOPropertyMatch</key>"
                 "<dict><key>Partition ID</key><integer>1</integer></dict></dict>")


class DeviceTree:
    """Flattened Apple DT, edited in place. iBoot's DT reserves every slot it fills; add() is for the rest."""

    def __init__(self, blob):
        self.buf, self.props, self.nodes, self.ends = bytearray(blob), {}, {}, {}
        end = self._node(0, None)
        assert end == len(blob), "trailing bytes after the device tree"

    def _node(self, off, parent):
        node = off
        nprops, nchildren = struct.unpack_from("<II", self.buf, off)
        off, props = off + 8, {}
        for _ in range(nprops):
            name = self.buf[off:off + 32].split(b"\0", 1)[0].decode()
            ln = struct.unpack_from("<I", self.buf, off + 32)[0] & 0x7FFFFFFF
            props[name] = (off, ln)
            off += 36 + ((ln + 3) & ~3)
        noff, nln = props["name"]
        name = bytes(self.buf[noff + 36:noff + 36 + nln]).split(b"\0", 1)[0].decode()
        path = "" if parent is None else f"{parent}/{name}".lstrip("/")
        self.props[path], self.nodes[path] = props, (node, off)
        for _ in range(nchildren):
            off = self._node(off, path)
        self.ends[path] = off
        return off

    def set(self, path, prop, value):
        off, ln = self.props[path][prop]
        if isinstance(value, str):
            value = value.encode() + b"\0"
        elif isinstance(value, int):
            value = struct.pack("<I", value)
        elif not isinstance(value, bytes):
            value = struct.pack(f"<{len(value)}I", *value)
        assert len(value) <= ln, f"{path}:{prop} holds {ln} bytes, got {len(value)}"
        self.buf[off + 36:off + 36 + ln] = value.ljust(ln, b"\0")

    def add(self, path, prop, value=b""):
        """Append a property to a node; the blob grows, so lay memory out after the last add()."""
        node, end = self.nodes[path]
        rec = prop.encode().ljust(32, b"\0") + struct.pack("<I", len(value)) + value.ljust((len(value) + 3) & ~3, b"\0")
        self.buf[end:end] = rec
        struct.pack_into("<I", self.buf, node, struct.unpack_from("<I", self.buf, node)[0] + 1)
        self.__init__(bytes(self.buf))

    def add_node(self, parent, name, props=()):
        """Append a child node (name first, then (prop, value) pairs) to `parent`; the blob grows, as add()."""
        rec = b"".join(self._prop(k, v) for k, v in [("name", name), *props])
        start = self.ends[parent]
        self.buf[start:start] = struct.pack("<II", 1 + len(props), 0) + rec
        node = self.nodes[parent][0]
        struct.pack_into("<I", self.buf, node + 4, struct.unpack_from("<I", self.buf, node + 4)[0] + 1)
        self.__init__(bytes(self.buf))

    @staticmethod
    def _prop(name, value):
        if isinstance(value, str):
            value = value.encode() + b"\0"
        elif isinstance(value, int):
            value = struct.pack("<I", value)
        elif not isinstance(value, bytes):
            value = struct.pack(f"<{len(value)}I", *value)
        return name.encode().ljust(32, b"\0") + struct.pack("<I", len(value)) + value.ljust((len(value) + 3) & ~3, b"\0")

    def rename(self, path, old, new):
        off, ln = self.props[path].pop(old)
        self.buf[off:off + 32] = new.encode().ljust(32, b"\0")[:32]
        self.props[path][new] = (off, ln)


def macho_entry(data):
    ncmds, off = struct.unpack_from("<I", data, 16)[0], 28
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == 5:  # LC_UNIXTHREAD, ARM_THREAD_STATE: r0..r15 start at +16
            return struct.unpack_from("<I", data, off + 16 + 15 * 4)[0]
        off += size
    raise ValueError("no LC_UNIXTHREAD")


def dt_board(dt):
    """The BOARDS entry for this DT, from its compatible ("N81AP\0iPod4,1\0AppleARM" -> n81)."""
    off, ln = dt.props[""].get("compatible", (None, 0))
    first = bytes(dt.buf[off + 36:off + 36 + ln]).split(b"\0", 1)[0].decode().lower() if off else ""
    return BOARDS.get(first[:-2] if first.endswith("ap") else first, BOARDS["k48"])


# K48's spi0/nor-flash subtree as its 4.x (8C148) DT has it (the 1 MiB SPI NOR: diagnostics, nvram, the image
# area, effaceable storage), for boards that keep all of that in NAND (boot-from-nand). The kernels carry both
# paths; grafting the NOR lets these boards use the machine's NOR model unchanged. Phandles are K48's.
NOR_GRAFT = [
    ("arm-io/spi0", "nor-flash", [("compatible", "nor-flash,spi"), ("#address-cells", 1), ("device_type", "nor-flash"),
                                  ("#size-cells", 1), ("ranges", (0, 0, 0x100000)),
                                  ("reg", (0, 0x53, 0x08010000, 0, 0, 0, 0, 0)), ("AAPL,phandle", 0x009170e0)]),
    ("arm-io/spi0/nor-flash", "diagnostic-data", [("compatible", "diagnostic-data,format1"),
                                                  ("device_type", "diagnostic-data"),
                                                  ("reg", (0x6000, 0x2000, 0x4000, 0x2000)), ("AAPL,phandle", 0x009174f0)]),
    ("arm-io/spi0/nor-flash", "nvram", [("compatible", "nvram,chrp"), ("device_type", "nvram"),
                                        ("reg", (0xfc000, 0x2000, 0xfe000, 0x2000)), ("AAPL,phandle", 0x00917880)]),
    ("arm-io/spi0/nor-flash", "raw-device", [("compatible", "raw-device,non-nvram"), ("device_type", "raw-device"),
                                             ("reg", (0x8000, 0xf2000, 0, 0x1000)), ("AAPL,phandle", 0x00917860)]),
    ("arm-io/spi0/nor-flash", "effaceable", [("compatible", "effaceable,nor"), ("device_type", "effaceable"),
                                             ("reg", (0xfa000, 0x1000, 0xfb000, 0x1000)), ("AAPL,phandle", 0x00917f70)]),
]


def graft_nor(dt):
    """Give a NOR-less board K48's NOR: the nodes above, and the NAND no longer the boot/nvram device."""
    if "arm-io/spi0" not in dt.props or "arm-io/spi0/nor-flash" in dt.props:
        return
    for parent, name, props in NOR_GRAFT:
        dt.add_node(parent, name, props)
    # The kernel keys off the property's presence (IOFlashStorageDevice then looks for boot blocks in
    # NAND), not its value; the editor cannot delete, so rename it out of the way.
    if "boot-from-nand" in dt.props.get("arm-io/flash-controller0/disk", {}):
        dt.rename("arm-io/flash-controller0/disk", "boot-from-nand", "boot-from-nor")


def fill_dt(dt, memory_map, ident, iboot=IBOOT_VERSION, root_matching=ROOT_MATCHING):
    board = dt_board(dt)
    root, chosen, macs = identity_dt(ident)
    for key, value in {"platform-name": "s5l8930x", **root}.items():
        dt.set("", key, value)
    # debug-enabled is forced (a production iBoot writes 0) so AMFI and PE_i_can_has_debugger honour boot-args.
    # display-rotation (iBoot: its video rotation byte x 90) is the panel's turn against the portrait UI.
    # 3.2.x ignores it; 4.x (MobileGestalt main-screen-orientation) lays the UI out by it, and only 270
    # makes 4.2.1 turn the UI for each accelerometer attitude exactly as 3.2.2 does on the real unit
    # (0 drew a landscape UI when held upright; 90 drew it upside down). docs/ipad1/ios4.md.
    for key, value in {"debug-enabled": 1, "production-cert": 1, "secure-boot": 1, "gid-aes-key": 1,
                       "uid-aes-key": 1, "system-trusted": 1, "board-id": board["board-id"], "chip-id": 0x8930,
                       **chosen, "firmware-version": iboot, "display-rotation": board["rotation"],
                       "display-scale": board["scale"],
                       "root-matching": root_matching}.items():
        dt.set("chosen", key, value)
    for key, hz in {"clock-frequency": CPU_HZ, "memory-frequency": MEM_HZ, "bus-frequency": BUS_HZ,
                    "peripheral-frequency": PERIPH_HZ, "fixed-frequency": FIXED_HZ,
                    "timebase-frequency": TIMEBASE_HZ}.items():
        dt.set("cpus/cpu0", key, hz)
    dt.set("arm-io", "clock-frequencies", CLOCKS)
    dt.set("arm-io", "usbphy-frequency", USBPHY_HZ)
    # No SGX model yet: kill the IMGSGX535 match so it never waits on the GPU, and
    # CoreAnimation falls back to its software renderer (docs/research/userland-gl-display.md).
    if "arm-io/sgx" in dt.props:
        dt.set("arm-io/sgx", "compatible", "none")
    for path, mac in macs.items():
        path = next((p for p in dt.props if p == path or p.endswith("/" + path)), path)
        if path in dt.props:  # absent from the selfcheck DT
            dt.set(path, "local-mac-address", mac)
    if "arm-io/mipi-dsim/lcd" in dt.props:
        # iBoot's pinot_init writes the panel's DCS 0xB1 reply here; ApplePinotLCD::start fails
        # on 0, AppleCLCD then never publishes, and CoreAnimation only finds AppleRGBOUT
        # (SpringBoard died in its status bar with a 240-wide TV-out screen). Any nonzero
        # id works: nothing looks it up. This is the reply the DSI model gives.
        for key in ("lcd-panel-id", "raw-panel-id"):
            dt.set("arm-io/mipi-dsim/lcd", key, 0x00A1D13C)
    # The K48 DT ships an N82 baseband node; on a Wi-Fi iPad iBoot finds no radio ("Radio not
    # detected.") and the real unit's IORegistry has no baseband node at all, so AppleBaseband never
    # loads and CommCenter never reports a dead radio. The editor cannot delete a node, so unmatch
    # and unname it instead. (spi2/uart2 stay: the real unit runs BasebandSPI/umts on them too.)
    # A radio board (N90) keeps its baseband node: the machine's baseband property unmatches it at boot.
    if "baseband" in dt.props and not board.get("radio"):
        for key, value in {"compatible": "none", "device_type": "none", "name": "nobb"}.items():
            dt.set("baseband", key, value)
    if "chip-revision" in dt.props["arm-io"]:  # absent from the selfcheck DT
        dt.set("arm-io", "chip-revision", 0x11)  # measured on the real K48AP
    # iBoot-1219 (5.x) DTs carry the geometry on flash-controller0 itself as well, plus ce-bitmap: the
    # populated CEs numbered across the buses (bus b's at 8b + n), which AppleIOPFMI-49 reads before its
    # first command (_fmiInitVirtToPhysMap loops forever on an empty one). Fill whichever node has the key.
    ces = NAND["#ce"] // NAND["#databus"]
    fill = dict(NAND, **{"ce-bitmap": sum(((1 << ces) - 1) << (8 * b) for b in range(NAND["#databus"]))})
    for node in ("arm-io/flash-controller0", "arm-io/flash-controller0/disk"):
        if node in dt.props:  # absent from the selfcheck's synthetic DT
            for key, value in fill.items():
                if key in dt.props[node]:   # iBoot-931 (4.x) DTs drop the *-ns timings
                    dt.set(node, key, value)
    _, vram_pa, pram_pa = layout(board)
    dt.set("pram", "reg", (pram_pa, PRAM_SIZE))
    dt.set("vram", "reg", (vram_pa, VRAM_SIZE))
    for i, (name, pa, size) in enumerate(memory_map):
        dt.rename("chosen/memory-map", f"MemoryMapReserved-{i}", name)
        dt.set("chosen/memory-map", name, (pa, size))


def boot_args_version(m):
    """The boot_args.Version pe_identify_machine demands, read off the kernel: the Thumb pair
    `ldrh rN, [r0, #2]` (0x8840|N) ... `cmp rN, #V` (0x2800|N<<8|V) just before the literal that
    names "pe_identify_machine: Epoch Mismatch". 2 when the shape is not found (3.2.x and 4.2.1,
    which boot with 2); 4.3's xnu-1735 and iOS 5's xnu-1878 say 3."""
    data = m.data
    so = data.find(b"pe_identify_machine: Epoch Mismatch")
    if so < 0:
        return 2
    sva = next(vmaddr + (so - fileoff) for _, vmaddr, _, fileoff, filesize, _ in m.segs
               if fileoff <= so < fileoff + filesize)
    lit = data.find(struct.pack("<I", sva))
    window = data[max(0, lit - 0x400):lit]
    for n in range(8):
        i = window.rfind(bytes([0x40 | n, 0x88]))
        if i < 0:
            continue
        j = window.find(bytes([0x28 | n]), i + 2, i + 10)
        if j > 0:
            return window[j - 1]
    return 2


def build(kernel_path, dt_blob, boot_args=DEFAULT_BOOT_ARGS, ident=None, iboot=IBOOT_VERSION, ramdisk=None):
    """Return (image bytes, load_pa, entry_pa, bootargs_pa). ramdisk: raw-HFS bytes to boot as md0."""
    page = lambda n: (n + 0xFFF) & ~0xFFF
    m = Macho(kernel_path)
    segs = [s for s in m.segs if s[0] != "__PAGEZERO"]
    # gVirtBase is the kernel's own link base: 3.x links at 0xC0000000, 4.x at 0x80000000
    vbase = min(vmaddr for _, vmaddr, _, _, _, _ in segs) & 0xF0000000
    pa = lambda va: va - vbase + PHYS_BASE
    dt = DeviceTree(dt_blob)
    board = dt_board(dt)
    graft_nor(dt)
    # Host nubs (EHCI, OHCI0) up at arbitrator start and kept across cable changes, next to device
    # mode: AppleS5L8930XUSBArbitrator::handleStart c04826a8 (docs/ipad1/usb-keyboard.md).
    if "arm-io/usb-complex" in dt.props:
        dt.add("arm-io/usb-complex", "hsic-enabled")
    dt_blob = bytes(dt.buf)
    top = page(max(vmaddr + vmsize for _, vmaddr, vmsize, _, _, _ in segs))
    rd_va = top
    if ramdisk is not None:
        top += page(len(ramdisk))
        boot_args += " rd=md0"
    dt_va, args_va = top, top + page(len(dt_blob))
    end_va = args_va + 0x1000
    top_of_kernel = pa((end_va + 0x3FFF) & ~0x3FFF)

    image = bytearray(end_va - vbase)
    memory_map = []
    for name, vmaddr, vmsize, fileoff, filesize, _ in segs:
        n = min(filesize, vmsize)
        image[vmaddr - vbase:vmaddr - vbase + n] = m.data[fileoff:fileoff + n]
        memory_map.append((f"Kernel-{name}", pa(vmaddr), vmsize))
    if ramdisk is not None:
        image[rd_va - vbase:rd_va - vbase + len(ramdisk)] = ramdisk
        memory_map.append(("RAMDisk", pa(rd_va), len(ramdisk)))
    memory_map += [("DeviceTree", pa(dt_va), len(dt_blob)), ("BootArgs", pa(args_va), 0x1000)]

    fill_dt(dt, memory_map, ident if ident is not None else load_identity(), iboot,
            ROOT_MATCHING if ramdisk is None else "")
    image[dt_va - vbase:dt_va - vbase + len(dt.buf)] = dt.buf

    # boot_args rev 1 / version 2 (pe_identify_machine c01d1276 panics otherwise) for the iBoot-817/931
    # kernels; the iBoot-1219 (iOS 5) kernels demand version 3, read off the kernel itself. Video depth word:
    # byte0 depth, byte1 rotation/90, byte2 scale-1. v_display 0 = text console, as iBoot sets for -v/-s.
    verbose = any(a in ("-v", "-s") for a in boot_args.split())
    cmdline = boot_args.encode()
    assert len(cmdline) < 256, "boot-args longer than BOOT_LINE_LENGTH"
    fbw, fbh = board["fb"]
    mem_size, vram_pa, _ = layout(board)
    args = struct.pack("<HHIIII6IIII256s", 1, boot_args_version(m), vbase, PHYS_BASE, mem_size, top_of_kernel,
                       vram_pa, 0 if verbose else 1, fbw * FB_DEPTH // 8, fbw, fbh,
                       FB_DEPTH | (board["scale"] - 1) << 16,
                       0, dt_va, len(dt_blob), cmdline)
    image[args_va - vbase:args_va - vbase + len(args)] = args
    return bytes(image), PHYS_BASE, pa(macho_entry(m.data)), pa(args_va)


def pack_segments(segments):
    return b"".join(SEGMENT.pack(b"K48SEG\0\0", pa, n, data is None) + (data or b"")
                    for pa, n, data in segments)


def main(dec_dir, out, boot_args=DEFAULT_BOOT_ARGS, identity=IDENTITY_FILE, ramdisk=None):
    dt_blob = open(os.path.join(dec_dir, "DeviceTree.bin"), "rb").read()
    image, load_pa, entry_pa, args_pa = build(os.path.join(dec_dir, "kernelcache.mach"), dt_blob, boot_args,
                                              load_identity(identity), iboot_version(dec_dir),
                                              open(ramdisk, "rb").read() if ramdisk else None)
    logo = os.path.join(dec_dir, "AppleLogo.bin")
    board = dt_board(DeviceTree(dt_blob))
    segments = logo_segments(open(logo, "rb").read(), layout(board)[1], board) \
        if os.path.exists(logo) else []
    with open(out, "wb") as f:
        f.write(image + pack_segments(segments) + TRAILER.pack(b"K48KBOOT", load_pa, entry_pa, args_pa, len(image)))
    top = struct.unpack_from("<I", image, args_pa - load_pa + 0x10)[0]
    boot_args += " rd=md0" if ramdisk else ""
    print(f"load {load_pa:#x}+{len(image):#x} entry {entry_pa:#x} r0 {args_pa:#x} "
          f"topOfKernelData {top:#x} boot-args [{boot_args}]")


def selfcheck():
    """Build from a two-segment Mach-O and a minimal DT, then check the loader-visible results."""
    def prop(name, value):
        return name.encode().ljust(32, b"\0") + struct.pack("<I", len(value)) + value.ljust((len(value) + 3) & ~3, b"\0")

    def node(props, children=()):
        return struct.pack("<II", len(props), len(children)) + b"".join(prop(*p) for p in props) + b"".join(children)

    z = lambda n: bytes(n)
    dt_blob = node([("name", b"device-tree\0")] + [(k, z(32)) for k in
                   ("platform-name", "model-number", "region-info", "serial-number", "mlb-serial-number")], [
        node([("name", b"chosen\0"), ("firmware-version", z(256)), ("root-matching", z(256)),
              ("unique-chip-id", z(8)), ("die-id", z(8))] + [(k, z(4)) for k in
             ("debug-enabled", "production-cert", "secure-boot", "gid-aes-key", "uid-aes-key", "system-trusted",
              "board-id", "chip-id", "display-rotation", "display-scale")],
             [node([("name", b"memory-map\0")] + [(f"MemoryMapReserved-{i}", z(8)) for i in range(16)])]),
        node([("name", b"cpus\0")], [node([("name", b"cpu0\0")] + [(k, z(4)) for k in
             ("clock-frequency", "memory-frequency", "bus-frequency", "peripheral-frequency",
              "fixed-frequency", "timebase-frequency")])]),
        node([("name", b"arm-io\0"), ("clock-frequencies", z(256)), ("usbphy-frequency", z(4))],
             [node([("name", b"usb-complex\0")], [node([("name", b"usb-ehci\0")])])]),
        node([("name", b"pram\0"), ("reg", z(8))]),
        node([("name", b"vram\0"), ("reg", z(8))]),
    ])

    def seg(name, vmaddr, vmsize, fileoff, filesize):
        return struct.pack("<II16s8I", 1, 56, name.encode(), vmaddr, vmsize, fileoff, filesize, 7, 7, 0, 0)

    def kernel_at(base, ramdisk=None):   # 3.x kernels link at 0xC0000000, 4.x at 0x80000000
        thread = struct.pack("<IIII16I", 5, 16 + 64, 1, 16, *([0] * 15), base + 0x1040)
        cmds = seg("__TEXT", base + 0x1000, 0x2000, 0, 0x2000) + seg("__DATA", base + 0x3000, 0x1800, 0x2000, 0x10) + thread
        kernel = bytearray(0x2010)
        kernel[:28 + len(cmds)] = struct.pack("<7I", 0xFEEDFACE, 12, 9, 2, 3, len(cmds), 0) + cmds
        kernel[0x2000:0x2010] = b"D" * 16
        with tempfile.NamedTemporaryFile() as f:
            f.write(kernel)
            f.flush()
            return build(f.name, dt_blob, ident=PLACEHOLDER, ramdisk=ramdisk)

    image4, _, entry4, r4 = kernel_at(0x80000000)
    # RAM-disk mode: the disk sits after the kernel (0x80004800 -> page 0x80005000), DT follows it
    rd = b"H+" * 0x900
    imgr, _, _, rr = kernel_at(0x80000000, rd)
    assert imgr[0x5000:0x5000 + len(rd)] == rd and rr == 0x40008000
    assert imgr[rr - 0x40000000 + 0x38:].split(b"\0", 1)[0] == (DEFAULT_BOOT_ARGS + " rd=md0").encode()
    dtr = DeviceTree(imgr[0x7000:0x7000 + len(dt_blob) + 36])
    getr = lambda path, key, fmt="<I": struct.unpack_from(fmt, dtr.buf, dtr.props[path][key][0] + 36)
    assert getr("chosen/memory-map", "RAMDisk", "<II") == (0x40005000, len(rd))
    assert bytes(dtr.buf[dtr.props["chosen"]["root-matching"][0] + 36:][:4]) == bytes(4)
    assert (entry4, r4) == (0x40001040, 0x40006000) and struct.unpack_from("<I", image4, r4 - 0x40000000 + 4)[0] == 0x80000000
    image, load, entry, r0 = kernel_at(0xC0000000)

    assert (load, entry, r0) == (0x40000000, 0x40001040, 0x40006000)
    assert image[0x1000:0x1004] == b"\xce\xfa\xed\xfe" and image[0x3000:0x3010] == b"D" * 16
    assert image[0x3010:0x4800] == bytes(0x17F0)  # __DATA zero fill past filesize
    rev, ver, vbase, pbase, memsize, tokd = struct.unpack_from("<HHIIII", image, r0 - load)
    assert (rev, ver, vbase, pbase, memsize, tokd) == (1, 2, 0xC0000000, 0x40000000, 0x0F700000, 0x40008000)
    dtp, dtlen = struct.unpack_from("<II", image, r0 - load + 0x30)
    assert (dtp, dtlen) == (0xC0005000, len(dt_blob) + 36)   # + hsic-enabled
    assert image[r0 - load + 0x38:].split(b"\0", 1)[0] == DEFAULT_BOOT_ARGS.encode()
    assert struct.unpack_from("<I", image, r0 - load + 0x18)[0] == 1  # no -v: graphics (the logo) stays up
    dt = DeviceTree(image[dtp - 0xC0000000:dtp - 0xC0000000 + dtlen])
    get = lambda path, key, fmt="<I": struct.unpack_from(fmt, dt.buf, dt.props[path][key][0] + 36)
    assert get("chosen/memory-map", "Kernel-__TEXT", "<II") == (0x40001000, 0x2000)
    assert get("chosen/memory-map", "DeviceTree", "<II") == (0x40005000, dtlen)
    assert dt.props["arm-io/usb-complex"]["hsic-enabled"][1] == 0 and "arm-io/usb-complex/usb-ehci" in dt.props
    assert get("chosen/memory-map", "BootArgs", "<II") == (0x40006000, 0x1000)
    assert "MemoryMapReserved-4" in dt.props["chosen/memory-map"]
    assert get("chosen", "unique-chip-id", "<II") == (1, 0) and get("chosen", "die-id", "<II") == (0, 0)
    assert identity_dt(PLACEHOLDER)[2]["arm-io/sdio"] == bytes.fromhex("020000000001")
    assert get("chosen", "chip-id") == (0x8930,) and get("cpus/cpu0", "timebase-frequency") == (24_000_000,)
    assert get("vram", "reg", "<II") == (0x4F700000, 0x8FC000) and get("pram", "reg", "<II") == (0x4FFFC000, 0x4000)
    a, b = synth_identity("x"), synth_identity("y")
    assert a == synth_identity("x") and a["udid"] != b["udid"] and len(a["serial-number"]) == 11
    assert int(a["bt-mac"].replace(":", ""), 16) == int(a["wifi-mac"].replace(":", ""), 16) + 1
    assert a["wifi-mac"].startswith("02:") and int(a["unique-chip-id"], 16) < 1 << 40
    assert identity_dt(a)[0]["model-number"] == "MB292"

    # Logo: a 2x1 iBootIm, left pixel opaque white, right transparent; all-literal LZSS stream.
    raw = bytes([255, 0, 255, 255])
    blob = (b"iBootIm\0" + bytes(4) + b"sszl" + b"yerg" + struct.pack("<HH", 2, 1)).ljust(0x40, b"\0")
    blob += bytes([0xFF]) + raw
    assert lzss(blob[0x40:]) == raw
    # an overlapping match: literal "ab", then 6 bytes from 2 back -> "abababab"
    assert lzss(bytes([0b011, ord("a"), ord("b"), 0xEE, 0xF3])) == b"abababab"
    (fb, fb_len, zero), (pa, n, rows) = logo_segments(blob, 0x4F700000)
    assert (fb, fb_len, zero) == (0x4F700000, 1024 * 768 * 4, None) and n == 1024 * 4 * 2
    # turned a quarter counter-clockwise: the logo's top row becomes its left column, its left end the bottom
    x0, y0 = (1024 - 1) // 2, (768 - 2) // 2
    assert pa == 0x4F700000 + y0 * 4096
    assert struct.unpack_from("<I", rows, x0 * 4)[0] == 0xFF000000      # row 0 <- logo x 1 (clear)
    assert struct.unpack_from("<I", rows, 4096 + x0 * 4)[0] == 0xFFFFFFFF   # row 1 <- logo x 0 (white)


if __name__ == "__main__":
    selfcheck()
    argv = sys.argv[1:]
    if argv[:1] == ["--synth-identity"] and len(argv) == 3:
        fd = os.open(argv[2], os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            json.dump(synth_identity(argv[1]), f, indent=1)
        sys.exit()
    identity, ramdisk = IDENTITY_FILE, None
    while argv[:1] in (["--identity"], ["--ramdisk"]) and len(argv) > 1:
        if argv[0] == "--identity":
            identity = argv[1]
        else:
            ramdisk = argv[1]
        argv = argv[2:]
    if len(argv) in (2, 3):
        main(*argv, identity=identity, ramdisk=ramdisk)
    elif argv:
        sys.exit(__doc__)
