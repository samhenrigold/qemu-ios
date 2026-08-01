#!/usr/bin/env python3
"""Regression harness for the emulated iPod touch 2G.

Every check here corresponds to a bug that actually shipped in this tree, so
none of them is a smoke test: "did it boot?" would have missed all of them.

  boot        SpringBoard reaches the home screen, judged from the *framebuffer*
              and not from a serial banner. A machine that prints the whole boot
              log and then shows a black screen is a regression the log cannot
              see. The panel's samples are very dark, so the frame is counted by
              lit sub-pixels rather than looked at: the boot logo is around 10k
              lit and the home screen around 290k, so the two are impossible to
              confuse.
  afc         Push and pull files over AFC and compare SHA-256, at sizes that
              are deliberately *not* multiples of 512. A residue bug in the USB
              bulk-IN path corrupted exactly the trailing partial packet, and
              round, 512-aligned test files never saw it.
  usbtcp      TCP-over-USB forwarding: iproxy to lockdownd on 62078 and a real
              lockdown QueryType round trip. This exercises the mux's stream
              layer rather than a one-shot service request.
  wifi        The BCM4325 model associates ("AirPort: Link Up on en0") *and* the
              guest takes a DHCP lease - checked on the wire, from the netdev
              capture, because a carrier with no lease looks identical in the log.
  appinstall  ideviceinstaller installs a decrypted App Store app and it appears
              in the installed list.
  applaunch   The freshly installed icon is located by diffing the home screen
              before and after the install, tapped, and the app is confirmed on
              screen. Requires appinstall.
  persist     A file written over AFC survives a *clean* shutdown and a reboot on
              the same overlay, byte-identical. HFS+ holds catalog updates in
              memory, so killing QEMU loses the directory entry while keeping the
              data blocks: the file silently ceases to exist. Only a
              system_powerdown unmounts the volume.
  fsck        The base image composed with the overlay fscks clean, i.e. the
              persisted pages landed on the physical pages the FTL mapping says
              they should.

Usage:
    tests/ipod/regress.py --files-dir ~/Developer/qemu-ios-files
    tests/ipod/regress.py --quick            # boot + afc only, one boot
    tests/ipod/regress.py --checks boot,wifi

Exits non-zero if any selected check fails or errors.

Open finding, found by this harness on `integration` and reproducible:

    tests/ipod/run-regression.sh --checks appinstall,applaunch   -> PASS
    tests/ipod/run-regression.sh --checks afc,appinstall         -> FAIL
    tests/ipod/run-regression.sh                                 -> FAIL

An app installs and launches perfectly on a device that has done nothing else.
Run the AFC round-trip first and the very same install wedges at
"Install: ExtractingPackage (15%)": installd resets the mux connection, no app
is registered, /var/mobile/Applications stays empty, and `ideviceinstaller`
waits forever, because it has no timeout of its own and sits in
idevice_wait_for_command_to_complete for a completion status that never comes.
Prior AFC traffic breaks a later app install - which is exactly the kind of
action-at-a-distance failure this harness exists to catch, so appinstall is left
in the default set as a failing check rather than reordered out of trouble.
--install-timeout bounds the hang so the rest of the suite still runs.
"""

import argparse
import hashlib
import json
import os
import plistlib
import shutil
import socket
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))

W, H = 320, 480
FRAME_BYTES = W * H * 3

# Lit sub-pixels. Measured on this tree: boot logo ~9.8k, a half-composited home
# screen (logo still drawn over icons that have already landed) ~121k, a settled
# home screen ~214k. The gate has to clear the *half-drawn* frame, not just the
# logo - the first version of this harness passed at 121k and called a boot
# finished while SpringBoard was still painting.
HOME_LIT_MIN = 180000
# And the frame has to stay there: one confirming sample this many seconds later
# must also be above the gate, so a transient cannot be mistaken for a boot.
HOME_CONFIRM_S = 15
LIT_THRESHOLD = 8

ALL_CHECKS = ["boot", "afc", "usbtcp", "wifi", "appinstall", "applaunch",
              "persist", "fsck"]
QUICK_CHECKS = ["boot", "afc"]

APP_BUNDLE_ID = "com.barackobama.Obama08"
APP_IPA_DEFAULT = os.path.expanduser(
    "~/Downloads/random apps/com.barackobama.Obama08-iOS2.0-(Clutch-2.0.4).ipa")


# --------------------------------------------------------------------------
# small utilities
# --------------------------------------------------------------------------

def log(msg):
    sys.stdout.write("[%7.1fs] %s\n" % (time.time() - START, msg))
    sys.stdout.flush()


def free_port(lo, hi):
    """Claim a free TCP port inside a range, so concurrent runs never collide."""
    for p in range(lo, hi + 1):
        s = socket.socket()
        try:
            s.bind(("127.0.0.1", p))
            return p
        except OSError:
            continue
        finally:
            s.close()
    raise RuntimeError("no free port in %d-%d" % (lo, hi))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def read_ppm(path):
    """Return (width, height, bytearray of samples) for a binary PPM."""
    with open(path, "rb") as f:
        data = f.read()
    parts, i = [], 0
    while len(parts) < 4:
        while data[i:i + 1].isspace():
            i += 1
        if data[i:i + 1] == b"#":
            while data[i:i + 1] != b"\n":
                i += 1
            continue
        start = i
        while not data[i:i + 1].isspace():
            i += 1
        parts.append(data[start:i])
    i += 1
    w, h = int(parts[1]), int(parts[2])
    return w, h, bytearray(data[i:i + w * h * 3])


def lit_count(path):
    """(max sample, number of lit sub-pixels). Screendumps are extremely dark."""
    try:
        _w, _h, pix = read_ppm(path)
    except Exception:
        return 0, 0
    if not pix:
        return 0, 0
    return max(pix), sum(1 for v in pix if v > LIT_THRESHOLD)


def to_png(ppm, png):
    """Brighten and convert, so a human can look at a failure afterwards."""
    try:
        w, h, pix = read_ppm(ppm)
        hi = max(pix) if pix else 0
        if 0 < hi < 255:
            scale = 255.0 / hi
            pix = bytearray(min(255, int(v * scale)) for v in pix)
        tmp = ppm + ".norm.ppm"
        with open(tmp, "wb") as f:
            f.write(b"P6\n%d %d\n255\n" % (w, h) + bytes(pix))
        subprocess.run(["sips", "-s", "format", "png", tmp, "--out", png],
                       capture_output=True, timeout=60)
        os.remove(tmp)
    except Exception:
        pass


# --------------------------------------------------------------------------
# QMP
# --------------------------------------------------------------------------

class QMP:
    """One long-lived QMP connection.

    QEMU's QMP listener accepts a single client at a time; a second connection
    is accepted by the kernel and then never answered, which looks exactly like
    a guest hang. Everything the harness does over QMP therefore goes through
    one instance of this class, and it is closed before contrib/it-poweroff.sh
    (which opens its own) is invoked.
    """

    def __init__(self, port, timeout=180):
        deadline = time.time() + timeout
        while True:
            try:
                self.s = socket.create_connection(("127.0.0.1", port), 5)
                break
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.5)
        self.s.settimeout(60)
        self.f = self.s.makefile("rwb")
        self._read()
        self.cmd("qmp_capabilities")

    def _read(self):
        while True:
            line = self.f.readline()
            if not line:
                raise EOFError("qmp closed")
            msg = json.loads(line)
            if "event" in msg:
                continue
            return msg

    def cmd(self, name, **args):
        req = {"execute": name}
        if args:
            req["arguments"] = args
        self.f.write((json.dumps(req) + "\n").encode())
        self.f.flush()
        r = self._read()
        if "error" in r:
            raise RuntimeError("%s: %s" % (name, r["error"]))
        return r.get("return")

    def shot(self, path):
        self.cmd("screendump", filename=path)
        for _ in range(60):
            if os.path.exists(path) and os.path.getsize(path) >= FRAME_BYTES:
                break
            time.sleep(0.1)
        return path

    def _abs(self, x, y):
        return (max(0, min(32767, int(x / W * 32768))),
                max(0, min(32767, int(y / H * 32768))))

    def tap(self, x, y, hold=0.15):
        ax, ay = self._abs(x, y)
        move = [{"type": "abs", "data": {"axis": "x", "value": ax}},
                {"type": "abs", "data": {"axis": "y", "value": ay}}]
        self.cmd("input-send-event", events=move)
        self.cmd("input-send-event", events=move + [
            {"type": "btn", "data": {"button": "left", "down": True}}])
        time.sleep(hold)
        self.cmd("input-send-event", events=[
            {"type": "btn", "data": {"button": "left", "down": False}}])

    def home(self):
        # The hardware buttons live behind the host Command modifier so plain
        # keys stay available for text entry: `key h` types an 'h'.
        self.cmd("send-key",
                 keys=[{"type": "qcode", "data": k}
                       for k in ("meta_l", "shift", "h")],
                 **{"hold-time": 250})

    def close(self):
        try:
            self.s.close()
        except OSError:
            pass


# --------------------------------------------------------------------------
# process management - only ever our own PIDs
# --------------------------------------------------------------------------

class Procs:
    def __init__(self):
        self.procs = []

    def spawn(self, argv, logpath, env=None):
        f = open(logpath, "wb")
        p = subprocess.Popen(argv, stdout=f, stderr=subprocess.STDOUT,
                             env=env, start_new_session=True)
        p._logfile = f
        self.procs.append(p)
        return p

    def stop(self, p, grace=8):
        if p is None or p.poll() is not None:
            return
        p.terminate()
        deadline = time.time() + grace
        while time.time() < deadline and p.poll() is None:
            time.sleep(0.2)
        if p.poll() is None:
            p.kill()
        try:
            p.wait(timeout=5)
        except Exception:
            pass

    def stop_all(self):
        for p in reversed(self.procs):
            self.stop(p)
        self.procs = []


# --------------------------------------------------------------------------
# the emulated device, plus its host-side usbmux bridge
# --------------------------------------------------------------------------

class Device:
    def __init__(self, cfg, procs, tag):
        self.cfg = cfg
        self.procs = procs
        self.tag = tag
        self.dir = os.path.join(cfg.out, tag)
        os.makedirs(self.dir, exist_ok=True)
        self.serial = os.path.join(self.dir, "serial.log")
        self.pcap = os.path.join(self.dir, "wifi.pcap")
        self.qmp = None
        self.qemu = None
        self.mux = None

    # -- lifecycle ---------------------------------------------------------

    def start(self):
        cfg = self.cfg
        muxcfg = os.path.join(cfg.out, "muxcfg")
        os.makedirs(muxcfg, exist_ok=True)
        env = dict(os.environ)
        env["USBMUXD_QEMU_ADDR"] = "127.0.0.1:%d" % cfg.usb_port
        env["USBMUXD_QEMU_DELAY"] = "12"
        self.mux = self.procs.spawn(
            [cfg.usbmuxd, "-f", "-v", "-v", "-v",
             "-S", "127.0.0.1:%d" % cfg.mux_port, "-P", "NONE", "-C", muxcfg],
            os.path.join(self.dir, "usbmuxd.log"), env=env)
        log("%s: usbmuxd pid %d (mux %d, usb %d)"
            % (self.tag, self.mux.pid, cfg.mux_port, cfg.usb_port))
        time.sleep(2)

        machine = ("iPod-Touch,bootrom=%s/bootrom_240_4,nand=%s,nor=%s/"
                   "nor_n72ap.bin,nandrw=%s,usb-attached=on,"
                   "usb-patch-mux-gate=on,usb-tcp-addr=127.0.0.1:%d"
                   % (cfg.files, cfg.base_nand, cfg.files, cfg.overlay,
                      cfg.usb_port))
        argv = [cfg.qemu, "-M", machine + ",wifi=on",
                "-cpu", "max", "-m", "2G", "-display", "none",
                "-serial", "file:" + self.serial,
                "-qmp", "tcp:127.0.0.1:%d,server=on,wait=off" % cfg.qmp_port,
                "-netdev", "user,id=wifi0,net=10.0.2.0/24,host=10.0.2.2,"
                           "dhcpstart=10.0.2.15",
                "-object", "filter-dump,id=cap0,netdev=wifi0,file=" + self.pcap]
        self.qemu = self.procs.spawn(argv, os.path.join(self.dir, "qemu.log"))
        log("%s: qemu pid %d (qmp %d)" % (self.tag, self.qemu.pid, cfg.qmp_port))
        self.qmp = QMP(cfg.qmp_port, timeout=180)

    def alive(self):
        return self.qemu is not None and self.qemu.poll() is None

    def powerdown(self):
        """Clean shutdown via contrib/it-poweroff.sh.

        Killing QEMU instead would leave HFS+ catalog updates in guest memory,
        so files written this boot would not exist on the next one - which is
        precisely what the persist check is here to detect.
        """
        if self.qmp:
            self.qmp.close()
            self.qmp = None
        r = subprocess.run(
            [os.path.join(ROOT, "contrib", "it-poweroff.sh"),
             "127.0.0.1:%d" % self.cfg.qmp_port, "180"],
            capture_output=True, text=True, timeout=300)
        log("%s: poweroff rc=%d %s" % (self.tag, r.returncode,
                                       r.stdout.strip().replace("\n", " | ")))
        return r.returncode == 0 and not self.alive()

    # -- boot --------------------------------------------------------------

    def wait_for_home(self, timeout):
        """Poll the framebuffer until the home screen is up.

        Polled rather than slept: a boot is ~3 minutes unloaded and much longer
        with other emulators on the machine, so any fixed sleep is either a
        false failure or a waste. The loop still fails hard if QEMU exits.
        """
        deadline = time.time() + timeout
        best = 0
        shot = os.path.join(self.dir, "boot.ppm")
        n = 0
        while time.time() < deadline:
            if not self.alive():
                return False, "qemu exited (rc=%s)" % self.qemu.returncode, best
            time.sleep(10)
            n += 1
            try:
                self.qmp.shot(shot)
            except Exception as e:
                return False, "screendump failed: %s" % e, best
            hi, lit = lit_count(shot)
            best = max(best, lit)
            if n % 3 == 0 or lit > HOME_LIT_MIN // 2:
                log("%s: t+%.0fs max=%d lit=%d"
                    % (self.tag, time.time() - START, hi, lit))
            if lit >= HOME_LIT_MIN:
                time.sleep(HOME_CONFIRM_S)
                if not self.alive():
                    return False, "qemu exited right after the home screen", best
                self.qmp.shot(shot)
                _hi2, lit2 = lit_count(shot)
                best = max(best, lit2)
                if lit2 < HOME_LIT_MIN:
                    log("%s: home screen did not hold (%d -> %d), still waiting"
                        % (self.tag, lit, lit2))
                    continue
                to_png(shot, os.path.join(self.dir, "home.png"))
                return True, "lit=%d, held for %ds" % (lit2, HOME_CONFIRM_S), best
        return False, "timed out after %ds" % timeout, best

    def serial_text(self):
        try:
            with open(self.serial, "rb") as f:
                return f.read().decode("utf-8", "replace")
        except OSError:
            return ""


# --------------------------------------------------------------------------
# host-side device tools
# --------------------------------------------------------------------------

def mux_env(cfg):
    env = dict(os.environ)
    env["USBMUXD_SOCKET_ADDRESS"] = "127.0.0.1:%d" % cfg.mux_port
    return env


def run(argv, cfg, timeout, stdin=None):
    """Run a libimobiledevice tool against *our* mux.

    A timeout is reported as a failed run rather than raised: one tool wedging
    should fail its own check and let the rest of the suite continue, not abort
    the whole run. macOS has no timeout(1), and subprocess's own deadline is
    both portable and exact, so no perl alarm wrapper is needed.
    """
    try:
        return subprocess.run(argv, env=mux_env(cfg), input=stdin,
                              timeout=timeout, capture_output=True, text=True)
    except subprocess.TimeoutExpired:
        return subprocess.CompletedProcess(
            argv, 124, "", "timed out after %ss" % timeout)


def wait_for_device(cfg, timeout=420):
    """Poll until the mux reports a device and lockdown answers.

    The guest needs ~100s of boot before it has programmed the USB core, and
    pairing can only happen after that, so this polls rather than assuming.
    """
    deadline = time.time() + timeout
    last = ""
    while time.time() < deadline:
        try:
            r = run(["idevice_id", "-l"], cfg, 30)
            if r.stdout.strip():
                udid = r.stdout.split()[0]
                p = run(["idevicepair", "pair"], cfg, 120)
                last = (p.stdout + p.stderr).strip()
                if "SUCCESS" in last.upper() or "success" in last:
                    return udid, last
                # already paired counts too
                v = run(["idevicepair", "validate"], cfg, 60)
                if "SUCCESS" in (v.stdout + v.stderr).upper():
                    return udid, "already paired"
        except subprocess.TimeoutExpired:
            last = "timeout"
        except Exception as e:
            last = str(e)
        time.sleep(10)
    return None, last


def afc(cfg, commands, timeout=300):
    return run(["afcclient"], cfg, timeout,
               stdin="".join(c + "\n" for c in commands) + "quit\n")


# --------------------------------------------------------------------------
# checks
# --------------------------------------------------------------------------

class Result:
    def __init__(self, name):
        self.name = name
        self.ok = None          # None = skipped
        self.detail = ""

    def set(self, ok, detail):
        self.ok = ok
        self.detail = detail
        log("  %-11s %s  %s" % (self.name, "PASS" if ok else "FAIL", detail))
        return ok


def check_afc(cfg, dev, r):
    """Round-trip files over AFC and compare SHA-256.

    Sizes are chosen so that most are *not* multiples of 512: the USB bulk-IN
    residue bug corrupted only the trailing short packet, so 4096-byte test
    files passed while 65535-byte ones came back wrong.
    """
    sizes = [1000, 65535, 4096, 1, 262143]
    tmpdir = os.path.join(cfg.out, "afc")
    os.makedirs(tmpdir, exist_ok=True)
    bad = []
    for n in sizes:
        src = os.path.join(tmpdir, "s%d.bin" % n)
        dst = os.path.join(tmpdir, "r%d.bin" % n)
        with open(src, "wb") as f:
            f.write(os.urandom(n))
        remote = "/regress_%d.bin" % n
        p = afc(cfg, ["put -f %s %s" % (src, remote)])
        if p.returncode != 0:
            bad.append("%d: put rc=%d %s" % (n, p.returncode,
                                             (p.stdout + p.stderr)[-160:]))
            continue
        g = afc(cfg, ["get -f %s %s" % (remote, dst)])
        if g.returncode != 0 or not os.path.exists(dst):
            bad.append("%d: get rc=%d %s" % (n, g.returncode,
                                             (g.stdout + g.stderr)[-160:]))
            continue
        got = os.path.getsize(dst)
        if got != n:
            bad.append("%d: got %d bytes" % (n, got))
        elif sha256_file(src) != sha256_file(dst):
            bad.append("%d: sha mismatch" % n)
        os.path.exists(dst) and os.remove(dst)
    if bad:
        return r.set(False, "; ".join(bad))
    return r.set(True, "sha256 identical at %s bytes"
                 % ",".join(str(s) for s in sizes))


def lockdown_querytype(port, timeout=30):
    """Speak lockdown over a forwarded TCP port: 4-byte length + XML plist."""
    s = socket.create_connection(("127.0.0.1", port), timeout)
    s.settimeout(timeout)
    try:
        req = plistlib.dumps({"Request": "QueryType"})
        s.sendall(struct.pack(">I", len(req)) + req)
        hdr = b""
        while len(hdr) < 4:
            b = s.recv(4 - len(hdr))
            if not b:
                raise IOError("closed reading length")
            hdr += b
        n = struct.unpack(">I", hdr)[0]
        if n > 1 << 20:
            raise IOError("absurd reply length %d" % n)
        body = b""
        while len(body) < n:
            b = s.recv(n - len(body))
            if not b:
                raise IOError("closed reading body")
            body += b
        return plistlib.loads(body)
    finally:
        s.close()


def check_usbtcp(cfg, dev, procs, r):
    lport = free_port(cfg.proxy_lo, cfg.proxy_hi)
    p = procs.spawn(["iproxy", "-s", "127.0.0.1", "%d:62078" % lport],
                    os.path.join(dev.dir, "iproxy.log"), env=mux_env(cfg))
    try:
        time.sleep(2)
        err = ""
        for _ in range(6):
            try:
                reply = lockdown_querytype(lport)
                if reply.get("Type") == "com.apple.mobile.lockdown":
                    return r.set(True, "iproxy %d->62078, QueryType -> %s"
                                 % (lport, reply["Type"]))
                err = "unexpected reply %r" % reply
            except Exception as e:
                err = "%s: %s" % (type(e).__name__, e)
            time.sleep(5)
        return r.set(False, err)
    finally:
        procs.stop(p)


def pcap_dhcp_reply(path):
    """Count BOOTP/DHCP replies (UDP sport 67) in a libpcap capture."""
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError:
        return 0
    if len(data) < 24:
        return 0
    magic = data[:4]
    if magic in (b"\xd4\xc3\xb2\xa1", b"\x4d\x3c\xb2\xa1"):
        endian = "<"
    elif magic in (b"\xa1\xb2\xc3\xd4", b"\xa1\xb2\x3c\x4d"):
        endian = ">"
    else:
        return 0
    i, n = 24, 0
    while i + 16 <= len(data):
        _ts, _us, caplen, _olen = struct.unpack(endian + "IIII", data[i:i + 16])
        i += 16
        pkt = data[i:i + caplen]
        i += caplen
        if len(pkt) < 42:
            continue
        if struct.unpack(">H", pkt[12:14])[0] != 0x0800:
            continue
        ihl = (pkt[14] & 0x0f) * 4
        if pkt[23] != 17:
            continue
        u = 14 + ihl
        if len(pkt) < u + 8:
            continue
        sport, dport = struct.unpack(">HH", pkt[u:u + 4])
        if sport == 67 and dport == 68:
            n += 1
    return n


def check_wifi(cfg, dev, r):
    """Carrier *and* lease.

    "AirPort: Link Up on en0" on its own only says the driver's state machine
    was satisfied; the data channel can still be dead. The DHCP reply on the
    netdev capture is the part that proves packets move both ways.
    """
    deadline = time.time() + 180
    link = False
    while time.time() < deadline:
        if "AirPort: Link Up on en0" in dev.serial_text():
            link = True
            break
        time.sleep(10)
    leases = 0
    deadline = time.time() + 120
    while time.time() < deadline:
        leases = pcap_dhcp_reply(dev.pcap)
        if leases:
            break
        time.sleep(10)
    if link and leases:
        return r.set(True, "Link Up on en0, %d DHCP reply/replies" % leases)
    return r.set(False, "link=%s dhcp_replies=%d" % (link, leases))


def check_appinstall(cfg, procs, dev, r):
    """Install an IPA and confirm installd really registered it.

    The install runs through a file so that its progress output survives being
    killed: `ideviceinstaller install` waits for a completion status from
    installd in an unbounded loop (`idevice_wait_for_command_to_complete`), so
    if installd resets the connection part-way the tool never returns, and the
    only evidence of how far it got is what it had already printed. The verdict
    comes from `ideviceinstaller list`, not from the installer's exit code:
    "the tool said OK" is weaker than "the device says the app is there".
    """
    if not os.path.exists(cfg.ipa):
        return r.set(False, "ipa not found: %s" % cfg.ipa)
    logpath = os.path.join(dev.dir, "install.log")
    p = procs.spawn(["ideviceinstaller", "install", cfg.ipa], logpath,
                    env=mux_env(cfg))
    deadline = time.time() + cfg.install_timeout
    while time.time() < deadline and p.poll() is None:
        time.sleep(5)
    timed_out = p.poll() is None
    if timed_out:
        procs.stop(p)
    try:
        with open(logpath) as f:
            out = " | ".join(l.strip() for l in f if l.strip())
    except OSError:
        out = ""
    l = run(["ideviceinstaller", "list"], cfg, 300)
    if APP_BUNDLE_ID in (l.stdout + l.stderr):
        return r.set(True, "%s installed and listed" % APP_BUNDLE_ID)
    if timed_out:
        return r.set(False, "installer never finished within %ds and the app "
                            "is not listed; last progress: %s"
                     % (cfg.install_timeout, out[-300:]))
    return r.set(False, "rc=%s, app not listed; %s"
                 % (p.returncode, out[-300:]))


def icon_centroid(before_ppm, after_ppm):
    """Where the newly installed icon appeared.

    Diffing the home screen before and after the install localises the icon
    without hard-coding a grid slot, which would depend on how many apps the
    image already ships. The status bar is excluded because its clock and WiFi
    indicator change on their own.
    """
    w, h, a = read_ppm(before_ppm)
    _w2, _h2, b = read_ppm(after_ppm)
    if len(a) != len(b):
        return None
    xs, ys, tot = 0, 0, 0
    for y in range(24, h - 20):
        row = y * w * 3
        for x in range(w):
            o = row + x * 3
            d = (abs(a[o] - b[o]) + abs(a[o + 1] - b[o + 1])
                 + abs(a[o + 2] - b[o + 2]))
            if d > 6:
                xs += x
                ys += y
                tot += 1
    if tot < 400:
        return None
    return xs // tot, ys // tot, tot


def frame_diff_fraction(p1, p2):
    _w, _h, a = read_ppm(p1)
    _w2, _h2, b = read_ppm(p2)
    if len(a) != len(b) or not a:
        return 1.0
    diff = sum(1 for i in range(0, len(a), 3) if abs(a[i] - b[i]) > 4)
    return diff / (len(a) / 3.0)


def check_applaunch(cfg, dev, before_ppm, r):
    after = dev.qmp.shot(os.path.join(dev.dir, "home-after-install.ppm"))
    to_png(after, os.path.join(dev.dir, "home-after-install.png"))
    spot = icon_centroid(before_ppm, after)
    if not spot:
        return r.set(False, "no new icon appeared on the home screen")
    x, y, npx = spot
    log("  new icon at (%d,%d), %d changed px" % (x, y, npx))
    dev.qmp.tap(x, y)
    time.sleep(30)
    shot = dev.qmp.shot(os.path.join(dev.dir, "app.ppm"))
    to_png(shot, os.path.join(dev.dir, "app.png"))
    hi, lit = lit_count(shot)
    frac = frame_diff_fraction(after, shot)
    if lit < 20000:
        return r.set(False, "screen went dark after tap (lit=%d)" % lit)
    if frac < 0.25:
        return r.set(False, "screen unchanged after tap (diff=%.2f) - still "
                            "on the home screen" % frac)
    return r.set(True, "app on screen: %.0f%% of the frame changed, lit=%d"
                 % (frac * 100, lit))


def check_persist(cfg, dev2, marker_src, remote, r):
    dst = os.path.join(cfg.out, "afc", "persist-back.bin")
    if os.path.exists(dst):
        os.remove(dst)
    g = afc(cfg, ["get -f %s %s" % (remote, dst)])
    if not os.path.exists(dst):
        return r.set(False, "file gone after reboot: %s"
                     % (g.stdout + g.stderr).strip()[-200:])
    if sha256_file(dst) != sha256_file(marker_src):
        return r.set(False, "file survived but the contents changed")
    return r.set(True, "%s identical after clean shutdown + reboot" % remote)


def check_fsck(cfg, clean_stop, r):
    """Compose base + overlay into a flat volume and fsck it.

    imgtools/ftlmap.predict() is the single source of truth for the allocation
    block -> (cs, page) mapping; a dirty result means persisted writes landed on
    the wrong physical pages.
    """
    sys.path.insert(0, os.path.join(ROOT, "imgtools"))
    try:
        from ftlmap import predict
    except Exception as e:
        return r.set(False, "cannot import ftlmap: %s" % e)
    img = os.path.join(cfg.out, "volume.img")
    used = 0
    with open(img, "wb") as out:
        for b in range(128000):
            cs, p = predict(b)
            f = os.path.join(cfg.overlay, "cs%d" % cs, "%d.page" % p)
            if os.path.exists(f):
                used += 1
            else:
                f = os.path.join(cfg.base_nand, "cs%d" % cs, "%d.page" % p)
            with open(f, "rb") as pf:
                out.write(pf.read(4096))
    att = subprocess.run(["hdiutil", "attach", "-nomount", "-readonly",
                          "-noverify", img], capture_output=True, text=True,
                         timeout=300)
    dev = ""
    for tok in att.stdout.split():
        if tok.startswith("/dev/disk"):
            dev = tok
            break
    if not dev:
        return r.set(False, "hdiutil attach failed: %s" % att.stderr.strip())
    try:
        fs = subprocess.run(["fsck_hfs", "-n", dev], capture_output=True,
                            text=True, timeout=900)
    finally:
        subprocess.run(["hdiutil", "detach", dev], capture_output=True,
                       timeout=120)
    out = fs.stdout + fs.stderr
    tail = " | ".join(l.strip() for l in out.strip().split("\n")[-4:])
    if fs.returncode == 0:
        return r.set(True, "%d overlay pages in the volume; %s" % (used, tail))
    # A free-block-count mismatch after an *unclean* stop is bookkeeping, not
    # corruption: the volume header's counter is only rewritten on unmount.
    only_freeblocks = ("free blocks" in out.lower()
                       and "invalid" not in out.lower()
                       and "missing" not in out.lower())
    if not clean_stop and only_freeblocks:
        return r.set(True, "free-block count off after an unclean stop "
                           "(expected); %s" % tail)
    return r.set(False, "fsck_hfs rc=%d: %s" % (fs.returncode, tail))


# --------------------------------------------------------------------------
# driver
# --------------------------------------------------------------------------

def main():
    global START
    START = time.time()
    ap = argparse.ArgumentParser(
        description="Regression harness for the emulated iPod touch 2G",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="checks: " + ", ".join(ALL_CHECKS))
    ap.add_argument("--files-dir",
                    default=os.path.expanduser("~/Developer/qemu-ios-files"))
    ap.add_argument("--base-nand", default=None,
                    help="base NAND image dir (default <files-dir>/nand-canonical)")
    ap.add_argument("--qemu", default=os.path.join(ROOT, "build",
                                                   "qemu-system-arm"))
    ap.add_argument("--usbmuxd",
                    default=os.path.expanduser(
                        "~/Developer/usbmuxd-qemu/usbmuxd/src/usbmuxd"))
    ap.add_argument("--ipa", default=APP_IPA_DEFAULT)
    ap.add_argument("--out", default=None, help="run directory")
    ap.add_argument("--checks", default=None,
                    help="comma-separated subset (default: all)")
    ap.add_argument("--quick", action="store_true",
                    help="boot + afc only, single boot")
    ap.add_argument("--boot-timeout", type=int, default=900,
                    help="seconds to wait for the home screen (default 900; "
                         "boots are ~3 min unloaded and much longer under "
                         "contention)")
    ap.add_argument("--install-timeout", type=int, default=420,
                    help="seconds to wait for ideviceinstaller (default 420; "
                         "it has no internal timeout and hangs forever if "
                         "installd drops the connection)")
    ap.add_argument("--qemu-port-lo", type=int, default=1511)
    ap.add_argument("--qemu-port-hi", type=int, default=1519)
    ap.add_argument("--mux-port-lo", type=int, default=27311)
    ap.add_argument("--mux-port-hi", type=int, default=27319)
    ap.add_argument("--qmp-port-lo", type=int, default=28001)
    ap.add_argument("--qmp-port-hi", type=int, default=28019)
    ap.add_argument("--proxy-port-lo", type=int, default=28101)
    ap.add_argument("--proxy-port-hi", type=int, default=28119)
    ap.add_argument("--keep", action="store_true",
                    help="leave the run directory in place (default anyway)")
    cfg = ap.parse_args()

    if cfg.quick and cfg.checks:
        sys.exit("--quick and --checks are mutually exclusive")
    selected = QUICK_CHECKS if cfg.quick else (
        [c.strip() for c in cfg.checks.split(",")] if cfg.checks else ALL_CHECKS)
    for c in selected:
        if c not in ALL_CHECKS:
            sys.exit("unknown check %r; known: %s" % (c, ", ".join(ALL_CHECKS)))
    if "applaunch" in selected and "appinstall" not in selected:
        selected.insert(selected.index("applaunch"), "appinstall")
    # boot is a precondition for everything else: nothing can be measured on a
    # device that never came up, and reporting the rest as PASS would be a lie.
    if len(selected) > 1 and "boot" not in selected:
        selected.insert(0, "boot")

    cfg.files = os.path.expanduser(cfg.files_dir)
    cfg.base_nand = cfg.base_nand or os.path.join(cfg.files, "nand-canonical")
    cfg.out = cfg.out or os.path.join(
        os.environ.get("TMPDIR", "/tmp"),
        "itregress-%d-%d" % (os.getpid(), int(START)))
    os.makedirs(cfg.out, exist_ok=True)
    cfg.overlay = os.path.join(cfg.out, "overlay")
    if os.path.exists(cfg.overlay):
        shutil.rmtree(cfg.overlay)
    os.makedirs(cfg.overlay)

    for path, what in ((cfg.qemu, "qemu binary"),
                       (cfg.base_nand, "base NAND"),
                       (cfg.usbmuxd, "usbmuxd")):
        if not os.path.exists(path):
            sys.exit("missing %s: %s" % (what, path))

    cfg.usb_port = free_port(cfg.qemu_port_lo, cfg.qemu_port_hi)
    cfg.mux_port = free_port(cfg.mux_port_lo, cfg.mux_port_hi)
    cfg.qmp_port = free_port(cfg.qmp_port_lo, cfg.qmp_port_hi)
    cfg.proxy_lo, cfg.proxy_hi = cfg.proxy_port_lo, cfg.proxy_port_hi

    log("run dir   %s" % cfg.out)
    log("base nand %s" % cfg.base_nand)
    log("checks    %s" % ", ".join(selected))

    results = {c: Result(c) for c in selected}
    procs = Procs()
    clean_stop = False
    needs_second_boot = "persist" in selected
    marker_src = os.path.join(cfg.out, "persist-marker.bin")
    marker_remote = "/regress_persist.bin"

    try:
        # ---------------- first boot ----------------
        dev = Device(cfg, procs, "boot1")
        dev.start()
        ok, detail, best = dev.wait_for_home(cfg.boot_timeout)
        results["boot"].set(ok, detail if ok else
                            "%s (best lit=%d, need >=%d)"
                            % (detail, best, HOME_LIT_MIN))
        if not ok:
            return finish(results, procs, cfg)

        if "wifi" in selected:
            check_wifi(cfg, dev, results["wifi"])

        need_usb = any(c in selected for c in
                       ("afc", "usbtcp", "appinstall", "applaunch", "persist"))
        udid = None
        if need_usb:
            udid, pdetail = wait_for_device(cfg)
            log("usbmux: udid=%s (%s)" % (udid, pdetail))
            if not udid:
                for c in ("afc", "usbtcp", "appinstall", "applaunch",
                          "persist"):
                    if c in results:
                        results[c].set(False, "device never appeared on the "
                                              "mux: %s" % pdetail)
                return finish(results, procs, cfg)

        if "afc" in selected:
            check_afc(cfg, dev, results["afc"])
        if "usbtcp" in selected:
            check_usbtcp(cfg, dev, procs, results["usbtcp"])

        home_before = None
        if "appinstall" in selected:
            dev.qmp.home()
            time.sleep(3)
            home_before = dev.qmp.shot(
                os.path.join(dev.dir, "home-before-install.ppm"))
            to_png(home_before,
                   os.path.join(dev.dir, "home-before-install.png"))
            if check_appinstall(cfg, procs, dev, results["appinstall"]):
                time.sleep(20)          # SpringBoard adds the icon
                if "applaunch" in selected:
                    check_applaunch(cfg, dev, home_before,
                                    results["applaunch"])
            elif "applaunch" in selected:
                results["applaunch"].set(False, "install failed")

        if needs_second_boot:
            with open(marker_src, "wb") as f:
                f.write(os.urandom(65535))
            p = afc(cfg, ["put -f %s %s" % (marker_src, marker_remote)])
            if p.returncode != 0:
                results["persist"].set(
                    False, "could not write the marker: %s"
                    % (p.stdout + p.stderr)[-200:])
                needs_second_boot = False

        # ---------------- clean shutdown ----------------
        if needs_second_boot or "fsck" in selected:
            if "applaunch" in selected:
                dev.qmp.home()          # leave the app so SpringBoard settles
                time.sleep(5)
            clean_stop = dev.powerdown()
            if not clean_stop:
                log("powerdown did not complete; killing")
                procs.stop(dev.qemu)
        else:
            procs.stop(dev.qemu)
        procs.stop(dev.mux)
        time.sleep(3)

        # ---------------- second boot ----------------
        if needs_second_boot:
            if not clean_stop:
                results["persist"].set(
                    False, "the guest did not shut down cleanly, so the test "
                           "for surviving a clean shutdown cannot be run")
            else:
                dev2 = Device(cfg, procs, "boot2")
                dev2.start()
                ok2, d2, best2 = dev2.wait_for_home(cfg.boot_timeout)
                log("second boot: %s (%s, best lit=%d)"
                    % ("up" if ok2 else "FAILED", d2, best2))
                if not ok2:
                    results["persist"].set(
                        False, "second boot did not reach the home screen: %s"
                        % d2)
                else:
                    udid2, pd2 = wait_for_device(cfg)
                    if not udid2:
                        results["persist"].set(
                            False, "device did not reappear on the mux: %s"
                            % pd2)
                    else:
                        check_persist(cfg, dev2, marker_src, marker_remote,
                                      results["persist"])
                    clean_stop = dev2.powerdown()
                    if not clean_stop:
                        procs.stop(dev2.qemu)
                procs.stop(dev2.mux)
                time.sleep(2)

        if "fsck" in selected:
            check_fsck(cfg, clean_stop, results["fsck"])

    except Exception:
        # An exception is a harness failure, not a pass: print it, mark every
        # check that never produced a verdict as failed, and still tear down.
        import traceback
        traceback.print_exc()
        for r in results.values():
            if r.ok is None:
                r.set(False, "harness error before this check completed")

    return finish(results, procs, cfg)


def finish(results, procs, cfg):
    procs.stop_all()
    print("")
    print("=" * 62)
    failed = 0
    for name in ALL_CHECKS:
        r = results.get(name)
        if r is None:
            continue
        if r.ok is None:
            state = "SKIP"
        elif r.ok:
            state = "PASS"
        else:
            state = "FAIL"
            failed += 1
        print("%-4s  %-11s %s" % (state, r.name, r.detail))
    print("=" * 62)
    print("%d check(s) failed; artifacts in %s" % (failed, cfg.out))
    print("total runtime %.1f min" % ((time.time() - START) / 60.0))
    return 1 if failed else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
