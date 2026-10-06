#!/usr/bin/env python3
"""The iPod touch 1G's accelerometer controls over QOM: the names the app and the other boards use.

Without them the app's rotation and tilt never reached the 1G (qemu_ios_ui_orientation / _attitude
found no accel-* property): the guest and its LCD stayed portrait while the app turned the shell, so a
portrait frame was drawn as if pre-rotated (Sam, 3A101a, 10-04). The machine is paused (-S): this
checks the controls and the sensor vector the guest's AppleLIS302DL samples, not a boot.
    QEMU=build/qemu-system-arm tests/ipod/test_attitude_qmp_1g.py"""
import os, tempfile, time
from pathlib import Path
from types import SimpleNamespace
import regress as r
root = Path(__file__).resolve().parents[2]
files = Path(os.environ.get("QEMU_IOS_FILES", root.parent / "qemu-ios-files")) / "ipod1g"
out = tempfile.mkdtemp(prefix="it-attitude-1g-")
os.makedirs(out + "/overlay")
cfg = SimpleNamespace(out=out, board="n45ap", bootrom=str(files / "bootrom_s5l8900"), direct_iboot=str(files / "iboot_204_n45ap.bin"),
                      base_nand=str(files / "nand"), nor=str(files / "nor_n45ap.bin"), overlay=out + "/overlay",
                      qemu=os.environ.get("QEMU", str(root / "build/qemu-system-arm")), qmp_port=r.free_port(28220, 28239))


class Procs(r.Procs):
    def spawn(self, argv, *rest, **kwargs):
        return super().spawn(list(argv) + ["-S"] if argv[0] == cfg.qemu else argv, *rest, **kwargs)


r.START = time.time()
p = Procs(); d = r.Device(cfg, p, "device")
try:
    d.start(); q = d.qmp
    def get(name): return q.cmd("qom-get", path="/machine", property="accel-" + name)
    def put(name, value): return q.cmd("qom-set", path="/machine", property="accel-" + name, value=value)
    def vector(): return tuple(get(axis) for axis in "xyz")
    assert get("orientation") == 1 and vector() == (0, -64, 0), (get("orientation"), vector())
    # UIDeviceOrientation as the app sends it (EmulatorController.setAccelerometer): Home right is 3.
    put("orientation", 3); assert get("orientation") == 3 and vector() == (-64, 0, 0) and get("roll") == 90, vector()
    put("orientation", 4); assert vector() == (64, 0, 0) and get("roll") == -90
    put("orientation", 2); assert vector() == (0, 64, 0)
    put("orientation", 1); assert vector() == (0, -64, 0)
    # The app's tilt (qemu_ios_ui_attitude: pose, then pitch and roll).
    put("pose", "upright"); put("pitch", 0); put("roll", 90); assert vector() == (-64, 0, 0) and get("pose") == "upright"
    put("pose", "flat"); put("roll", 0); assert vector() == (0, 0, -64)
    for name, value in [("pose", "sideways"), ("pitch", 181), ("roll", -181)]:
        try: put(name, value)
        except RuntimeError: pass
        else: raise AssertionError("invalid attitude accepted: %s=%r" % (name, value))
    put("x", 1 << 40); assert get("x") == 127
    put("shake", True)
    d.qmp.close(); d.qmp = None; p.stop_all()
    # The iPhone (M68): the same controls, its part turned 180 degrees about Z to the N45's (DT orientation 0
    # where the N45's is 3, which negates x and y), so Home right (3) reads +x where the 1G's reads -x. With the
    # N45's mapping 1.0 turned its UI upside down in landscape (Sam, 10-06).
    import subprocess, sys
    sys.path.insert(0, str(root / "imgtools")); import itqmp
    sock = out + "/m68.qmp"
    m68 = subprocess.Popen([cfg.qemu, "-S", "-M", "iPhone-2G,bootrom=%s,iboot=%s,nand=%s,nand-overlay=%s,baseband=off"
                            % (cfg.bootrom, cfg.direct_iboot, cfg.base_nand, cfg.overlay), "-drive", "if=pflash,format=raw,readonly=on,file=" + cfg.nor,
                            "-display", "none", "-audio", "driver=none", "-serial", "null", "-qmp", "unix:%s,server,nowait" % sock])
    try:
        [time.sleep(0.5) for _ in range(40) if not os.path.exists(sock)]; q = itqmp.QMP(sock, timeout=30)
        assert vector() == (0, 64, 0), vector()
        put("orientation", 3); assert vector() == (64, 0, 0) and get("roll") == 90, vector()
        put("orientation", 4); assert vector() == (-64, 0, 0)
        put("pose", "flat"); put("roll", 0); assert vector() == (0, 0, -64), vector()
        q.close()
    finally:
        m68.kill(); m68.wait()
    print("PASS: the 1G takes accel-orientation, -pitch/-roll/-pose, raw axes and shake, as the app sends them; the M68 reads x and y turned")
finally:
    if d.qmp: d.qmp.close()
    p.stop_all()
