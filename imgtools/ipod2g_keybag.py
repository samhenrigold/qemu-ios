#!/usr/bin/env python3
"""The 4.x data-protection one-shot for an iPod touch 2G device: format effaceable storage and create the
system keybag, the step a restore's restored does (docs/ipod/from-ipsw.md, "Data protection").

    ipod2g_keybag.py OUT --dec DEC --ramdisk NAME [--qemu PATH] [--helper PATH]

OUT is a device made by device.py (nand/, nor.bin, iBoot.bin, gid-blobs.bin); DEC the decrypt cache holding
kernelcache.mach and the decrypted restore ramdisk NAME (raw HFS+). Mirrors imgtools/ipad1_keybag.py, except
for how the ramdisk reaches the kernel. Everything iBoot loads is an Apple-signed img3 (the ramdisk from
NVRAM `boot-ramdisk` too), so a ramdisk carrying the helper cannot come through iBoot. Instead iBoot boots the
device normally and, at the kernel's entry (LC_UNIXTHREAD pc, gdbstub breakpoint, MMU still off), this adds
what iBoot's restore path would have: the ramdisk at topOfKernelData, a chosen/memory-map RAMDisk (pa, len)
entry in a spare MemoryMapReserved slot, an empty chosen/root-matching and
topOfKernelData past the ramdisk, and the host-owned one-shot command line. The 4.x DeviceTree's own secure-root-prefix 'md' makes md0 a SecureRoot.

The ramdisk (a private copy) gets it_keybag (contrib/it-keybag/build-ipod.sh) as restored_external, which
rc.boot runs first: effaceable format (lands in NOR through nor-rw), MKBKeyBagCreateSystem into disk0s1's
/private/var (lands in the NAND overlay), halt. Then the overlay's pages are folded into OUT/nand (they are
stored at their logical homes, which are the image's own page files) and the written NOR replaces OUT/nor.bin.
The boot uses aes-uid=engine, as the device's own boots must (the keybag's keys are UID-derived).
"""
import argparse, os, shutil, socket, struct, subprocess, sys, tempfile, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from ipad1_keybag import DONE, ramdisk_with_helper
from ipad1_kboot import DeviceTree

FILES = os.path.expanduser("~/Developer/qemu-ios-files")
# The host stages these with the ramdisk at the paused kernel handoff.
# Stock iBoot identity population is left intact; no literal or timer is needed.
BOOT_ARGS = "rd=md0 serial=3 debug=0x8 -v amfi_allow_any_signature=1 cs_enforcement_disable=1"
CMDLINE_OFF, CMDLINE_LEN = 0x38, 256


class Gdb:
    """Just enough of the gdb remote protocol: breakpoints, registers, memory, continue."""

    def __init__(self, port):
        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", port), 5)
                break
            except OSError:
                time.sleep(0.1)
        else:
            raise SystemExit("no gdbstub on port %d" % port)
        self.buf = b""

    def _read(self):
        while True:
            i = self.buf.find(b"$")
            j = self.buf.find(b"#", i)
            if i >= 0 and j >= 0 and len(self.buf) >= j + 3:
                data, self.buf = self.buf[i + 1:j], self.buf[j + 3:]
                self.s.sendall(b"+")
                return data.decode("latin-1")
            chunk = self.s.recv(65536)
            if not chunk:
                raise SystemExit("gdbstub closed")
            self.buf += chunk

    def cmd(self, text, reply=True):
        pkt = text.encode("latin-1")
        self.s.sendall(b"$%s#%02x" % (pkt, sum(pkt) & 0xFF))
        return self._read() if reply else None

    def regs(self):
        g = self.cmd("g")
        return [struct.unpack("<I", bytes.fromhex(g[i * 8:i * 8 + 8]))[0] for i in range(16)]

    def read(self, addr, n):
        out = b""
        while len(out) < n:
            k = min(0x800, n - len(out))
            out += bytes.fromhex(self.cmd("m%x,%x" % (addr + len(out), k)))
        return out

    def write(self, addr, data):
        for off in range(0, len(data), 0x400):      # QEMU's packet buffer is 4 KiB of hex
            part = data[off:off + 0x400]
            r = self.cmd("M%x,%x:%s" % (addr + off, len(part), part.hex()))
            if r != "OK":
                raise SystemExit("gdb write at 0x%x: %s" % (addr + off, r))


def kernel_entry(kernelcache):
    """(entry pc, link base) from the kernel Mach-O: LC_UNIXTHREAD pc, __TEXT vmaddr's top nibble."""
    d = open(kernelcache, "rb").read()
    ncmds, off, pc, base = struct.unpack_from("<I", d, 16)[0], 28, None, None
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", d, off)
        if cmd == 5:
            pc = struct.unpack_from("<17I", d, off + 16)[15]
        if cmd == 1 and d[off + 8:off + 24].rstrip(b"\0") == b"__TEXT":
            base = struct.unpack_from("<I", d, off + 24)[0] & 0xF0000000
        off += size
    assert pc and base, "no LC_UNIXTHREAD / __TEXT in " + kernelcache
    return pc, base


def add_ramdisk(dt_blob, rd_pa, rd_len):
    """DeviceTree with a RAMDisk memory-map entry (in a spare reserved slot) and no root-matching."""
    dt = DeviceTree(dt_blob)
    spare = sorted(p for p in dt.props["chosen/memory-map"] if p.startswith("MemoryMapReserved-"))
    assert spare, "no spare chosen/memory-map slot"
    dt.rename("chosen/memory-map", spare[0], "RAMDisk")
    dt.set("chosen/memory-map", "RAMDisk", (rd_pa, rd_len))
    dt.set("chosen", "root-matching", b"")
    assert len(dt.buf) == len(dt_blob)
    return bytes(dt.buf)


def selfcheck():
    """add_ramdisk on a minimal DT: root > chosen (root-matching) > memory-map (two reserved slots)."""
    prop = lambda n, v: n.encode().ljust(32, b"\0") + struct.pack("<I", len(v)) + v.ljust((len(v) + 3) & ~3, b"\0")
    node = lambda props, kids=(): struct.pack("<II", len(props), len(kids)) + b"".join(prop(*p) for p in props) + b"".join(kids)
    mm = node([("name", b"memory-map\0"), ("DeviceTree", struct.pack("<II", 0x881b000, 0x981c)),
               ("MemoryMapReserved-0", bytes(8)), ("MemoryMapReserved-1", bytes(8))])
    blob = node([("name", b"device-tree\0")],
                [node([("name", b"chosen\0"), ("root-matching", b"<dict>partition 1</dict>".ljust(64, b"\0"))], [mm])])
    dt = DeviceTree(add_ramdisk(blob, 0x08828000, 0x1000000))
    off, ln = dt.props["chosen/memory-map"]["RAMDisk"]
    assert struct.unpack_from("<II", dt.buf, off + 36) == (0x08828000, 0x1000000)
    assert "MemoryMapReserved-1" in dt.props["chosen/memory-map"] and "MemoryMapReserved-0" not in dt.props["chosen/memory-map"]
    off, ln = dt.props["chosen"]["root-matching"]
    assert bytes(dt.buf[off + 36:off + 36 + ln]) == bytes(ln)


def handoff(gdb, kc, ramdisk):
    """Stop at the kernel's entry and give it the ramdisk the way iBoot's restore path does."""
    pc, base = kernel_entry(kc)
    entry = pc - base + 0x08000000
    assert gdb.cmd("Z0,%x,4" % entry) == "OK"
    stop = gdb.cmd("c")
    r = gdb.regs()
    assert stop.startswith("T") and r[15] == entry, "did not stop at the kernel entry: %s pc=%x" % (stop, r[15])
    gdb.cmd("z0,%x,4" % entry)
    ba = r[0]
    args = bytearray(gdb.read(ba, 0x138))
    rev, virt, phys, mem, top = struct.unpack_from("<HxxIIII", args, 0)
    dtp, dtlen = struct.unpack_from("<II", args, 0x30)
    assert rev == 1 and phys == 0x08000000 and virt == base, "unexpected boot_args %r" % (args[:16],)
    rd_pa = (top + 0xFFF) & ~0xFFF
    new_top = (rd_pa + len(ramdisk) + 0x3FFF) & ~0x3FFF
    assert new_top < phys + mem, "ramdisk does not fit below memSize"
    dt_pa = dtp - virt + phys
    gdb.write(dt_pa, add_ramdisk(gdb.read(dt_pa, dtlen), rd_pa, len(ramdisk)))
    gdb.write(rd_pa, ramdisk)
    # This host one-shot already stages the ramdisk and topOfKernelData. Own
    # its command line here too, before the first kernel instruction, rather
    # than relying on a firmware literal redirect or a timer race.
    command = BOOT_ARGS.encode()
    assert len(command) < CMDLINE_LEN, "keybag command line exceeds boot_args capacity"
    args[CMDLINE_OFF:CMDLINE_OFF + CMDLINE_LEN] = command.ljust(CMDLINE_LEN, b"\0")
    line = BOOT_ARGS
    struct.pack_into("<I", args, 0x10, new_top)
    gdb.write(ba, bytes(args))
    print("keybag boot: ramdisk %d bytes at 0x%08x, topOfKernelData 0x%08x -> 0x%08x, [%s]"
          % (len(ramdisk), rd_pa, top, new_top, line), flush=True)
    gdb.cmd("c", reply=False)


def fold_overlay(ovl, nand):
    """Overlay pages (stored at their logical homes) over the device's page files."""
    n = 0
    for cs in sorted(os.listdir(ovl)):
        mode = os.stat(os.path.join(nand, cs)).st_mode & 0o777
        os.chmod(os.path.join(nand, cs), mode | 0o200)
        for name in os.listdir(os.path.join(ovl, cs)):
            if name.endswith(".page") and not name.startswith("."):
                dst = os.path.join(nand, cs, name)
                if os.path.exists(dst):
                    os.chmod(dst, 0o644)
                shutil.copyfile(os.path.join(ovl, cs, name), dst)
                os.chmod(dst, 0o444)
                n += 1
            elif name.endswith(".erased"):
                raise SystemExit("erase markers in the overlay (FMSS_ERASE set?); not folding")
        os.chmod(os.path.join(nand, cs), mode)
    return n


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("out")
    ap.add_argument("--dec", required=True)
    ap.add_argument("--ramdisk", required=True, help="decrypted restore ramdisk file name in DEC")
    ap.add_argument("--helper", default=os.path.join(ROOT, "build/ipod-guest/it_keybag"))
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--keep", action="store_true", help="keep the work directory (serial, qemu log)")
    a = ap.parse_args()
    out = os.path.abspath(a.out)
    td = tempfile.mkdtemp(prefix="ipod-keybag-")
    rd, nor, ovl, serial = f"{td}/ramdisk.dmg", f"{td}/nor.rw", f"{td}/ovl", f"{td}/serial.log"
    ramdisk_with_helper(os.path.join(a.dec, a.ramdisk), a.helper, rd)
    shutil.copyfile(f"{out}/nor.bin", nor)
    os.chmod(nor, 0o644)
    os.makedirs(ovl)
    port = 20000 + os.getpid() % 20000
    machine = ",".join([f"iPod-Touch,bootrom={FILES}/bootrom_240_4", f"nand={out}/nand", f"nor={out}/nor.bin",
                        f"nor-rw={nor}", f"nandrw={ovl}", f"direct-iboot={out}/iBoot.bin",
                        f"gid-blobs={out}/gid-blobs.bin", "aes-uid=engine", "boot-args="])
    q = subprocess.Popen([a.qemu, "-M", machine, "-m", "128M", "-display", "none", "-audio", "driver=none",
                          "-serial", "file:" + serial, "-gdb", f"tcp:127.0.0.1:{port}", "-S"],
                         stdout=open(f"{td}/qemu.log", "w"), stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    try:
        handoff(Gdb(port), os.path.join(a.dec, "kernelcache.mach"), open(rd, "rb").read())
        q.wait(timeout=a.timeout)
    except subprocess.TimeoutExpired:
        pass
    finally:
        if q.poll() is None:
            q.kill()
            q.wait()
    text = open(serial, "rb").read().decode("latin-1").replace("\r", "")
    for line in text.splitlines():
        if "it_keybag:" in line:
            print(line.strip())
    if DONE not in text:
        sys.exit(f"keybag boot: no {DONE!r} (qemu exit {q.returncode}); serial in {serial}")
    if open(nor, "rb").read() == open(f"{out}/nor.bin", "rb").read():
        sys.exit(f"keybag boot: NOR unchanged, effaceable was not written; serial in {serial}")
    pages = fold_overlay(ovl, f"{out}/nand")
    os.chmod(f"{out}/nor.bin", 0o644)
    shutil.copyfile(nor, f"{out}/nor.bin")
    os.chmod(f"{out}/nor.bin", 0o444)
    print(f"keybag boot: effaceable in nor.bin, {pages} NAND pages folded in")
    if a.keep:
        print("work directory kept: " + td)
    else:
        shutil.rmtree(td, ignore_errors=True)


if __name__ == "__main__":
    selfcheck()
    main()
