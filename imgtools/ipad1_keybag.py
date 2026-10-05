#!/usr/bin/env python3
"""The 4.x data-protection one-shot: boot the IPSW's restore ramdisk once to format effaceable storage and
create the system keybag on a new device's NAND store + writable NOR (docs/ipad1/ios4.md).

    ipad1_keybag.py STORE NOR --dec DEC --ramdisk NAME [--identity FILE] [--die-id 0xW2:0xW3] [--qemu PATH] [--board n18]

DEC is ipad1_fw.py's output; NAME its decrypted restore ramdisk (raw HFS+, e.g. 038-0024-002-ramdisk.dmg).
A private copy of the ramdisk gets it_keybag (contrib/it-keybag) as /usr/local/bin/restored_external, the
first thing its rc.boot runs; kboot boots it as md0 with the stock restore kernelcache and DeviceTree,
whose secure-root-prefix 'md' makes the root a SecureRoot. it_keybag formats effaceable (lands in NOR),
creates /private/var/keybags/systembag.kb (lands in STORE) and halts. STORE and NOR are written in place.
--board n18/n88 boots the S5L8920 machine through s5l8920_kboot.py --nor (its NOR is the grafted one).
A boot that panics or does not halt is retried (up to ATTEMPTS) from a copy of STORE and NOR taken
before the first, with the reason and the panic line logged.
"""
import argparse, os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import build_nand as bn
import ipad1_kboot
from ipad1_rootfs import BLOCK, Mounted, grow_to_partition
from ipad1_seal import boot

DONE = "it_keybag: effaceable formatted, system keybag created"
HELPER = "usr/local/bin/restored_external"
ROOM = 1 << 20          # free space for the helper; the stock ramdisk is 100% full
ATTEMPTS = 3


def ramdisk_with_helper(src, helper, out):
    shutil.copyfile(src, out)
    grow_to_partition(out, (os.path.getsize(out) + ROOM + BLOCK - 1) // BLOCK)
    with Mounted(out, out + ".mnt") as m:
        dst = os.path.join(m.mnt, HELPER)
        shutil.copyfile(helper, dst)
        os.chmod(dst, 0o755)
    bn.set_owner(out, [HELPER], 0, 0)
    os.rmdir(out + ".mnt")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("store")
    ap.add_argument("nor")
    ap.add_argument("--dec", required=True)
    ap.add_argument("--ramdisk", required=True, help="decrypted restore ramdisk file name in DEC")
    ap.add_argument("--identity", default=ipad1_kboot.IDENTITY_FILE)
    ap.add_argument("--die-id")
    ap.add_argument("--helper", default=os.path.join(ROOT, "build/ipad1-guest/it_keybag"))
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build/qemu-system-arm"))
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--board", help="n18/n88 (-M n18/n88, S5L8920); A4 boards are read off the DeviceTree")
    ap.add_argument("--keep-temp", action="store_true", help="keep the temp tree (ramdisk, kboot, store clone; ~1.5 GB)")
    a = ap.parse_args()
    td = tempfile.mkdtemp(prefix="ipad1-keybag-")
    try:
        run(a, td)
    finally:
        if a.keep_temp:
            print(f"temp tree kept: {td}")
        else:
            shutil.rmtree(td, ignore_errors=True)


def run(a, td):
    # The serial log sits beside the store, so it outlives the temp tree a failure message points into.
    rd, kboot, serial = f"{td}/ramdisk.dmg", f"{td}/kboot-restore.bin", os.path.abspath(a.store).rstrip("/") + ".keybag.log"
    ramdisk_with_helper(os.path.join(a.dec, a.ramdisk), a.helper, rd)
    if a.board in ("n18", "n88"):
        import s5l8920_kboot
        s5l8920_kboot.main(a.board, a.dec, kboot, identity=a.identity, ramdisk=rd, nor=True)
        machine = a.board
    else:
        ipad1_kboot.main(a.dec, kboot, identity=a.identity, ramdisk=rd)
        machine = ipad1_kboot.dt_board(ipad1_kboot.DeviceTree(open(os.path.join(a.dec, "DeviceTree.bin"), "rb").read()))["machine"]
    nor = open(a.nor, "rb").read()
    extra = f"nand={os.path.abspath(a.store)},nor-rw={os.path.abspath(a.nor)}" + (f",die-id={a.die_id}" if a.die_id else "")
    pre = f"{td}/store.pre"
    subprocess.run(["cp", "-cR", a.store, pre], check=True)     # APFS clone: the retry's starting point
    for attempt in range(1, ATTEMPTS + 1):
        log = serial if attempt == 1 else f"{serial}.{attempt}"
        exited, t, text = boot(a.qemu, f"kboot={kboot}", extra, log, lambda s: "panic(" in s, a.timeout, machine)
        for line in text.splitlines():
            if line.startswith("it_keybag:"):
                print(line)
        if exited and DONE in text:
            break
        why = next((l[l.index("panic("):][:160] for l in text.splitlines() if "panic(" in l),
                   "no halt" if not exited else "halted without " + repr(DONE))
        print(f"keybag boot attempt {attempt}/{ATTEMPTS} failed after {t:.0f}s: {why}; serial in {log}", flush=True)
        if attempt == ATTEMPTS:
            sys.exit(f"keybag boot: {why}; serial in {log}")
        shutil.rmtree(a.store)
        subprocess.run(["cp", "-cR", pre, a.store], check=True)
        with open(a.nor, "wb") as f:
            f.write(nor)
    if open(a.nor, "rb").read() == nor:
        sys.exit(f"keybag boot: {a.nor} unchanged, effaceable was not written; serial in {serial}")
    print(f"keybag boot halted cleanly after {t:.0f}s")


if __name__ == "__main__":
    main()
