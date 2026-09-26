#!/usr/bin/env python3
"""Build a direct-kernel boot image for the ipad1 machine: what iBoot-817.29 does before it jumps to xnu.

    ipad1_kboot.py DEC_DIR OUT [BOOT_ARGS]

DEC_DIR is ipad1_fw.py's output (kernelcache.mach, DeviceTree.bin). BOOT_ARGS defaults to DEFAULT_BOOT_ARGS.
With no arguments only the self-check runs.

OUT format (all little-endian): a flat image of physical memory, then a 24-byte trailer.

    [0, image_len)      bytes to place at physical load_pa (0x40000000, DRAM base)
    trailer:  char magic[8] = "K48KBOOT"; u32 load_pa; u32 entry_pa; u32 bootargs_pa; u32 image_len

Loader contract: 256 MiB DRAM at 0x40000000. Copy the image (or the whole file; the trailer then lands in
padding nothing uses) to load_pa, then start the CPU at entry_pa in ARM state, SVC mode, IRQ/FIQ masked,
MMU and caches off, r0 = bootargs_pa, other registers 0. The kernel builds its first page table at
topOfKernelData (boot_args+0x10, 16 KiB aligned, just past the image) and zeroes 0x9000 bytes there.

Physical layout, mirroring iBoot's allocator (kernel VA 0xC0000000 = PA 0x40000000):
    kernel segments   PA = vmaddr - 0x80000000 (filesize copied, the rest of vmsize zeroed)
    DeviceTree        next page after the highest segment end
    BootArgs          next page after the DT (one page; the struct is 0x138 bytes)
    topOfKernelData   end of BootArgs rounded up to 16 KiB
    memSize           0x0F700000: DRAM less pram (0x4000) and vram (0x8FC000), as iBoot computes it
    vram              0x4F700000 + 0x8FC000 (iBoot writes the 0x5F700000 alias; same RAM through the mirror)
    pram              0x4FFFC000 + 0x4000   (iBoot writes 0x5FFFC000; ditto)
"""
import os, struct, sys, tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from macho import Macho

VIRT_BASE, PHYS_BASE, DRAM_SIZE = 0xC0000000, 0x40000000, 0x10000000
PRAM_SIZE, VRAM_SIZE = 0x4000, 0x900000 - 0x4000
MEM_SIZE = DRAM_SIZE - PRAM_SIZE - VRAM_SIZE
VRAM_PA, PRAM_PA = PHYS_BASE + MEM_SIZE, PHYS_BASE + DRAM_SIZE - PRAM_SIZE
FB_WIDTH, FB_HEIGHT, FB_DEPTH = 768, 1024, 32
# serial bit 0 moves the console to UART0 (arm_init c005d5fe); debug=0x8 is DB_KPRT, which PE_init_kprintf
# (c01d1dce) needs before kprintf reaches the UART. No rd=: root-matching below names partition 1.
DEFAULT_BOOT_ARGS = "-v serial=3 debug=0x8"
TRAILER = struct.Struct("<8sIIII")

# ponytail: clocks are guesses (timebase = the kernel's own 24 MHz default); replace with HW-2's real
# IODeviceTree values. clock-frequencies slots follow iBoot's clock_get_frequency (5ff10f80) indices.
# Measured on a real iPad 1 running 7B500 (sysctl hw.*, 2026-09-26): iBoot leaves cpu and
# memory frequency at 0; bus and peripheral are 100 MHz; fixed and timebase 24 MHz.
CPU_HZ, MEM_HZ, BUS_HZ, PERIPH_HZ, FIXED_HZ, TIMEBASE_HZ, USBPHY_HZ = (
    0, 0, 100_000_000, 100_000_000, 24_000_000, 24_000_000, 24_000_000)
# NAND geometry iBoot would have probed: the Samsung 0x7294D7EC part from the iBoot-817 K48 chip
# table (4 CE x 0x1038 blocks x 128 pages x 8 KiB = 16 GB). Must agree with the IOP model's nand-id
# and nand-ce-mask. Timings are placeholders the HLE ignores; ECC values are a guess.
NAND = {"#ce": 4, "#die-ce": 1, "#ce-blocks": 0x1038, "#block-pages": 128, "#page-bytes": 8192,
        "#spare-bytes": 0x1b4, "device-readid": 0x7294D7EC, "vendor-type": 0x100014, "#databus": 8,
        "ecc-correctable": 16, "ecc-threshold": 8, "bbt-format": 0,
        "read-cycle-ns": 25, "read-setup-ns": 10, "read-hold-ns": 10, "read-delay-ns": 20,
        "read-valid-ns": 20, "write-cycle-ns": 25, "write-hold-ns": 10}
CLOCKS = [PERIPH_HZ] * 55
for idx, hz in {0: TIMEBASE_HZ, 5: CPU_HZ, 6: PERIPH_HZ, 27: MEM_HZ, 32: BUS_HZ, 33: FIXED_HZ}.items():
    CLOCKS[idx] = hz

ROOT_MATCHING = ("<dict><key>IOProviderClass</key><string>IOMedia</string><key>IOPropertyMatch</key>"
                 "<dict><key>Partition ID</key><integer>1</integer></dict></dict>")


class DeviceTree:
    """Flattened Apple DT, edited in place. iBoot's DT reserves every slot it fills, so sizes never change."""

    def __init__(self, blob):
        self.buf, self.props = bytearray(blob), {}
        end = self._node(0, None)
        assert end == len(blob), "trailing bytes after the device tree"

    def _node(self, off, parent):
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
        self.props[path] = props
        for _ in range(nchildren):
            off = self._node(off, path)
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


def fill_dt(dt, memory_map):
    for key, value in {"platform-name": "s5l8930x", "model-number": "MB292", "region-info": "LL/A",
                       "serial-number": "QEMUIPAD1"}.items():
        dt.set("", key, value)
    # debug-enabled is forced (a production iBoot writes 0) so AMFI and PE_i_can_has_debugger honour boot-args.
    for key, value in {"debug-enabled": 1, "production-cert": 1, "secure-boot": 1, "gid-aes-key": 1,
                       "uid-aes-key": 1, "system-trusted": 1, "board-id": 0x02, "chip-id": 0x8930,
                       "unique-chip-id": (0x1D2A3B4C, 0x000000E8), "die-id": (0x1D2A3B4C, 0x000000E8),
                       "firmware-version": "iBoot-817.29", "display-rotation": 0, "display-scale": 1,
                       "root-matching": ROOT_MATCHING}.items():
        dt.set("chosen", key, value)
    for key, hz in {"clock-frequency": CPU_HZ, "memory-frequency": MEM_HZ, "bus-frequency": BUS_HZ,
                    "peripheral-frequency": PERIPH_HZ, "fixed-frequency": FIXED_HZ,
                    "timebase-frequency": TIMEBASE_HZ}.items():
        dt.set("cpus/cpu0", key, hz)
    dt.set("arm-io", "clock-frequencies", CLOCKS)
    dt.set("arm-io", "usbphy-frequency", USBPHY_HZ)
    if "arm-io/flash-controller0/disk" in dt.props:  # absent from the selfcheck's synthetic DT
        for key, value in NAND.items():
            dt.set("arm-io/flash-controller0/disk", key, value)
    dt.set("pram", "reg", (PRAM_PA, PRAM_SIZE))
    dt.set("vram", "reg", (VRAM_PA, VRAM_SIZE))
    for i, (name, pa, size) in enumerate(memory_map):
        dt.rename("chosen/memory-map", f"MemoryMapReserved-{i}", name)
        dt.set("chosen/memory-map", name, (pa, size))


def build(kernel_path, dt_blob, boot_args=DEFAULT_BOOT_ARGS):
    """Return (image bytes, load_pa, entry_pa, bootargs_pa)."""
    page = lambda n: (n + 0xFFF) & ~0xFFF
    pa = lambda va: va - VIRT_BASE + PHYS_BASE
    m = Macho(kernel_path)
    segs = [s for s in m.segs if s[0] != "__PAGEZERO"]
    top = page(max(vmaddr + vmsize for _, vmaddr, vmsize, _, _, _ in segs))
    dt_va, args_va = top, top + page(len(dt_blob))
    end_va = args_va + 0x1000
    top_of_kernel = pa((end_va + 0x3FFF) & ~0x3FFF)

    image = bytearray(end_va - VIRT_BASE)
    memory_map = []
    for name, vmaddr, vmsize, fileoff, filesize, _ in segs:
        n = min(filesize, vmsize)
        image[vmaddr - VIRT_BASE:vmaddr - VIRT_BASE + n] = m.data[fileoff:fileoff + n]
        memory_map.append((f"Kernel-{name}", pa(vmaddr), vmsize))
    memory_map += [("DeviceTree", pa(dt_va), len(dt_blob)), ("BootArgs", pa(args_va), 0x1000)]

    dt = DeviceTree(dt_blob)
    fill_dt(dt, memory_map)
    image[dt_va - VIRT_BASE:dt_va - VIRT_BASE + len(dt.buf)] = dt.buf

    # boot_args rev 1 / version 2 (pe_identify_machine c01d1276 panics otherwise). Video depth word:
    # byte0 depth, byte1 rotation/90, byte2 scale-1. v_display 0 = text console, as iBoot sets for -v/-s.
    verbose = any(a in ("-v", "-s") for a in boot_args.split())
    cmdline = boot_args.encode()
    assert len(cmdline) < 256, "boot-args longer than BOOT_LINE_LENGTH"
    args = struct.pack("<HHIIII6IIII256s", 1, 2, VIRT_BASE, PHYS_BASE, MEM_SIZE, top_of_kernel,
                       VRAM_PA, 0 if verbose else 1, FB_WIDTH * FB_DEPTH // 8, FB_WIDTH, FB_HEIGHT, FB_DEPTH,
                       0, dt_va, len(dt_blob), cmdline)
    image[args_va - VIRT_BASE:args_va - VIRT_BASE + len(args)] = args
    return bytes(image), PHYS_BASE, pa(macho_entry(m.data)), pa(args_va)


def main(dec_dir, out, boot_args=DEFAULT_BOOT_ARGS):
    dt_blob = open(os.path.join(dec_dir, "DeviceTree.bin"), "rb").read()
    image, load_pa, entry_pa, args_pa = build(os.path.join(dec_dir, "kernelcache.mach"), dt_blob, boot_args)
    with open(out, "wb") as f:
        f.write(image + TRAILER.pack(b"K48KBOOT", load_pa, entry_pa, args_pa, len(image)))
    top = struct.unpack_from("<I", image, args_pa - load_pa + 0x10)[0]
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
                   ("platform-name", "model-number", "region-info", "serial-number")], [
        node([("name", b"chosen\0"), ("firmware-version", z(256)), ("root-matching", z(256)),
              ("unique-chip-id", z(8)), ("die-id", z(8))] + [(k, z(4)) for k in
             ("debug-enabled", "production-cert", "secure-boot", "gid-aes-key", "uid-aes-key", "system-trusted",
              "board-id", "chip-id", "display-rotation", "display-scale")],
             [node([("name", b"memory-map\0")] + [(f"MemoryMapReserved-{i}", z(8)) for i in range(16)])]),
        node([("name", b"cpus\0")], [node([("name", b"cpu0\0")] + [(k, z(4)) for k in
             ("clock-frequency", "memory-frequency", "bus-frequency", "peripheral-frequency",
              "fixed-frequency", "timebase-frequency")])]),
        node([("name", b"arm-io\0"), ("clock-frequencies", z(256)), ("usbphy-frequency", z(4))]),
        node([("name", b"pram\0"), ("reg", z(8))]),
        node([("name", b"vram\0"), ("reg", z(8))]),
    ])

    def seg(name, vmaddr, vmsize, fileoff, filesize):
        return struct.pack("<II16s8I", 1, 56, name.encode(), vmaddr, vmsize, fileoff, filesize, 7, 7, 0, 0)

    thread = struct.pack("<IIII16I", 5, 16 + 64, 1, 16, *([0] * 15), 0xC0001040)
    cmds = seg("__TEXT", 0xC0001000, 0x2000, 0, 0x2000) + seg("__DATA", 0xC0003000, 0x1800, 0x2000, 0x10) + thread
    kernel = bytearray(0x2010)
    kernel[:28 + len(cmds)] = struct.pack("<7I", 0xFEEDFACE, 12, 9, 2, 3, len(cmds), 0) + cmds
    kernel[0x2000:0x2010] = b"D" * 16
    with tempfile.NamedTemporaryFile() as f:
        f.write(kernel)
        f.flush()
        image, load, entry, r0 = build(f.name, dt_blob)

    assert (load, entry, r0) == (0x40000000, 0x40001040, 0x40006000)
    assert image[0x1000:0x1004] == b"\xce\xfa\xed\xfe" and image[0x3000:0x3010] == b"D" * 16
    assert image[0x3010:0x4800] == bytes(0x17F0)  # __DATA zero fill past filesize
    rev, ver, vbase, pbase, memsize, tokd = struct.unpack_from("<HHIIII", image, r0 - load)
    assert (rev, ver, vbase, pbase, memsize, tokd) == (1, 2, 0xC0000000, 0x40000000, 0x0F700000, 0x40008000)
    dtp, dtlen = struct.unpack_from("<II", image, r0 - load + 0x30)
    assert (dtp, dtlen) == (0xC0005000, len(dt_blob))
    assert image[r0 - load + 0x38:].split(b"\0", 1)[0] == DEFAULT_BOOT_ARGS.encode()
    assert struct.unpack_from("<I", image, r0 - load + 0x18)[0] == 0  # -v selects the text console
    dt = DeviceTree(image[dtp - 0xC0000000:dtp - 0xC0000000 + dtlen])
    get = lambda path, key, fmt="<I": struct.unpack_from(fmt, dt.buf, dt.props[path][key][0] + 36)
    assert get("chosen/memory-map", "Kernel-__TEXT", "<II") == (0x40001000, 0x2000)
    assert get("chosen/memory-map", "DeviceTree", "<II") == (0x40005000, len(dt_blob))
    assert get("chosen/memory-map", "BootArgs", "<II") == (0x40006000, 0x1000)
    assert "MemoryMapReserved-4" in dt.props["chosen/memory-map"]
    assert get("chosen", "chip-id") == (0x8930,) and get("cpus/cpu0", "timebase-frequency") == (24_000_000,)
    assert get("vram", "reg", "<II") == (0x4F700000, 0x8FC000) and get("pram", "reg", "<II") == (0x4FFFC000, 0x4000)


if __name__ == "__main__":
    selfcheck()
    if len(sys.argv) in (3, 4):
        main(*sys.argv[1:])
    elif len(sys.argv) != 1:
        sys.exit(__doc__)
