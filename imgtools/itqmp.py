#!/usr/bin/env python3
"""Shared QMP client for driving the emulated iPod touch.

One transport, used by everything that used to hand-roll its own: the CLI
(`contrib/ipod-touch-qmp.py`), the recorder (`imgtools/record.py`), the kernel
log dumper (`imgtools/klog.py`), and `contrib/it-poweroff.sh`. Five copies of
the same ~20-line QMP handshake had already drifted: only one had multitouch,
only one had hardware buttons, only one retried the connect while QEMU is
still starting up.

LOCAL CLIENT, NOT upstream `python/qemu/qmp/`: that library is asyncio-based,
and every consumer here is a small blocking script (a CLI one-liner, a
screendump loop, a shell-invoked subcommand) that wants a synchronous
request/reply call, not an event loop. Wrapping each in asyncio would be more
code than this file, for no capability gained. This client has no external
dependency.

The LCD device registers a legacy absolute mouse handler
(`ipod_touch_lcd_mouse_event`, 0..0x7fff on both axes) and a multi-touch
handler ("mtt") for pinch/rotate. Hardware buttons sit behind the host
Command modifier so plain keys stay free for text entry (see `button()`).
"""

import base64
import uuid
import json
import math
import os
import socket
import subprocess
import sys
import time

W, H = 320, 480
ABS_MAX = 0x7FFF
RAM_BASE = 0x08000000
RAM_SIZE = 128 * 1024 * 1024


class QMP:
    """Blocking QMP client. Accepts (host, port) or a single "host:port" /
    unix-socket-path target string (the shapes `it-poweroff.sh` needs)."""

    def __init__(self, host, port=None, timeout=180):
        if port is None:
            if isinstance(host, int):
                host, port = "127.0.0.1", host
            elif ":" in host and not host.startswith("/"):
                host, port_s = host.rsplit(":", 1)
                port = int(port_s)
            else:
                self.s = self._connect_unix(host, timeout)
        if port is not None:
            self.s = self._connect_tcp(host, port, timeout)
        self.s.settimeout(timeout)
        self.f = self.s.makefile("rwb")
        self.shutdown_event = None
        self.reset_count = 0
        try:
            self._read()  # greeting
            self.cmd("qmp_capabilities")
        except BaseException:
            self.close()
            raise

    @staticmethod
    def _connect_tcp(host, port, timeout):
        deadline = time.time() + timeout
        while True:
            try:
                return socket.create_connection((host, port), 5)
            except OSError:
                if time.time() > deadline:
                    raise
                time.sleep(0.5)

    @staticmethod
    def _connect_unix(path, timeout):
        deadline = time.time() + timeout
        while True:
            s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            try:
                s.connect(path)
                return s
            except OSError:
                s.close()
                if time.time() > deadline:
                    raise
                time.sleep(0.5)

    def _read_message(self):
        line = self.f.readline()
        if not line:
            raise EOFError("QMP closed without a guest shutdown confirmation")
        msg = json.loads(line)
        if msg.get("event") == "SHUTDOWN" and self.shutdown_event is None:
            self.shutdown_event = msg
        if msg.get("event") == "RESET":
            self.reset_count += 1
        return msg

    def _read(self):
        while True:
            msg = self._read_message()
            if "event" not in msg:
                return msg

    def wait_for_guest_shutdown(self, timeout):
        """Require QEMU's guest-origin shutdown event, never EOF or exit status.

        This is a terminal wait: after a read timeout, socket.makefile cannot
        safely resume reading, so callers must close the connection.
        """
        deadline = time.monotonic() + timeout
        old_timeout = self.s.gettimeout()
        try:
            while self.shutdown_event is None:
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise TimeoutError("timed out waiting for guest SHUTDOWN event")
                self.s.settimeout(remaining)
                self._read_message()
            data = self.shutdown_event.get("data", {})
            if data.get("guest") is not True or data.get("reason") != "guest-shutdown":
                raise RuntimeError("shutdown was not guest-confirmed: %s" % data)
            return True
        finally:
            self.s.settimeout(old_timeout)

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

    def close(self):
        try:
            self.f.close()
        except OSError:
            pass
        finally:
            self.s.close()


def agent_alive(q):
    return q.cmd("qom-get", path="/machine", property="agent-status") == "alive"


def agent(q, op, args="", body=b"", timeout=65):
    """One local RPC; returns (exit_status, binary_output).

    A timeout is an unknown execution outcome, never a reason to retry a mutation.
    Results for other callers are retained on this QMP connection. Use one shared
    client per machine; competing QMP connections cannot consume each other's RPCs.
    """
    if any(c in op + args for c in "\r\n") or not op or " " in op:
        raise ValueError("invalid agent header")
    request_id = uuid.uuid4().hex
    header = request_id + " " + op + (" " + args if args else "")
    request = header + "\n" + base64.b64encode(body).decode("ascii")
    q.cmd("qom-set", path="/machine", property="agent-request", value=request)
    deadline = time.monotonic() + timeout
    pending = getattr(q, "_agent_results", None)
    if pending is None:
        pending = q._agent_results = {}
    completed = False
    try:
        while time.monotonic() < deadline:
            result = q.cmd("qom-get", path="/machine", property="agent-result")
            if result:
                header, encoded = result.split("\n", 1)
                identifier, status = header.split(" ", 1)
                value = (int(status), base64.b64decode(encoded, validate=True))
                if identifier == request_id:
                    completed = True
                    return value
                if len(pending) >= 64:
                    pending.pop(next(iter(pending)))
                pending[identifier] = value
            else:
                time.sleep(0.25)
        raise TimeoutError("agent request %s timed out; execution outcome unknown" % request_id)
    finally:
        if not completed:
            try:
                q.cmd("qom-set", path="/machine", property="agent-cancel", value=request_id)
            except (OSError, EOFError, RuntimeError):
                pass  # Preserve the original failure; never replay the command.



# ---------------------------------------------------------------------------
# Touch: single contact (tap/swipe) via the legacy absolute mouse handler.
# ---------------------------------------------------------------------------


def abs_xy(x, y):
    """Screen pixels -> the 0..0x7fff absolute axis values the LCD expects.

    Scaled against (W-1)/(H-1) so the last pixel reaches full scale exactly -
    touch is timing- and precision-sensitive near the screen edges (status
    bar taps, edge swipes), so this is not just int(x / W * 0x8000).
    """
    ax = int(x * ABS_MAX / (W - 1))
    ay = int(y * ABS_MAX / (H - 1))
    return max(0, min(ABS_MAX, ax)), max(0, min(ABS_MAX, ay))


def move(q, x, y):
    ax, ay = abs_xy(x, y)
    q.cmd("input-send-event", events=[
        {"type": "abs", "data": {"axis": "x", "value": ax}},
        {"type": "abs", "data": {"axis": "y", "value": ay}}])


def tap(q, x, y, hold=0.12):
    move(q, x, y)
    time.sleep(0.05)
    q.cmd("input-send-event", events=[
        {"type": "btn", "data": {"down": True, "button": "left"}}])
    time.sleep(hold)
    q.cmd("input-send-event", events=[
        {"type": "btn", "data": {"down": False, "button": "left"}}])


def swipe(q, x1, y1, x2, y2, steps=12, dt=0.02):
    move(q, x1, y1)
    q.cmd("input-send-event", events=[
        {"type": "btn", "data": {"down": True, "button": "left"}}])
    for i in range(1, steps + 1):
        move(q, x1 + (x2 - x1) * i // steps, y1 + (y2 - y1) * i // steps)
        time.sleep(dt)
    q.cmd("input-send-event", events=[
        {"type": "btn", "data": {"down": False, "button": "left"}}])


# ---------------------------------------------------------------------------
# Hardware buttons and raw keys.
# ---------------------------------------------------------------------------

# As the machine maps them (hw/arm/ipod_touch_2g.c).
BUTTONS = {
    "home":    ["meta_l", "shift", "h"],
    "power":   ["meta_l", "l"],
    "voldown": ["meta_l", "minus"],
    "volup":   ["meta_l", "equal"],
}


def button(q, name, hold_ms=300):
    """Press a hardware button as ORDERED key down/ups, NOT a chord.

    `send-key` presses the whole combination at once, and on an unlocked
    device the guest's keyboard sees the letter before the modifier has
    established that this is a button: a chord typed "Mm" into Spotlight
    instead of going Home. Pressing modifiers first, holding, then releasing
    in reverse order is what the machine's GPIO sampling actually expects -
    these are levels the guest polls, not edges, so the hold has to be long
    enough to be seen. Do not "simplify" this back into a single send-key.
    """
    keys = BUTTONS[name] if isinstance(name, str) else name
    down = [{"type": "key",
             "data": {"down": True, "key": {"type": "qcode", "data": k}}}
            for k in keys]
    up = [{"type": "key",
           "data": {"down": False, "key": {"type": "qcode", "data": k}}}
          for k in reversed(keys)]
    q.cmd("input-send-event", events=down)
    time.sleep(hold_ms / 1000.0)
    q.cmd("input-send-event", events=up)


def key(q, name, hold_ms=250):
    """Send a raw qcode key - NOT a hardware button, see `button()`.

    Plain 'h' types the letter h; a verification run once lost its whole
    sequence inside Safari's bookmarks over this exact confusion.
    """
    q.cmd("send-key", keys=[{"type": "qcode", "data": name}], **{"hold-time": hold_ms})


# ---------------------------------------------------------------------------
# Multi-touch: pinch/rotate need two simultaneous contacts, which the legacy
# mouse handler can't describe. The LCD also registers QEMU's "mtt" event
# kind, carrying a slot and tracking id per contact.
#
# Two-phase protocol: DATA events carry a slot's X/Y, then a following
# begin/update/end commits them. All five fields are mandatory in the QAPI
# schema even where ignored, hence the axis/value on the commit.
# ---------------------------------------------------------------------------


def _mtt(kind, slot, x=0, y=0):
    tid = slot + 1
    if kind == "data":
        return [
            {"type": "mtt", "data": {"type": "data", "slot": slot,
                                     "tracking-id": tid,
                                     "axis": "x", "value": x}},
            {"type": "mtt", "data": {"type": "data", "slot": slot,
                                     "tracking-id": tid,
                                     "axis": "y", "value": y}},
        ]
    return [{"type": "mtt", "data": {"type": kind, "slot": slot,
                                     "tracking-id": tid,
                                     "axis": "x", "value": x}}]


def finger(q, slot, x, y, phase):
    """Put contact `slot` at screen pixel (x, y). phase: begin/update/end."""
    ax, ay = abs_xy(x, y)
    q.cmd("input-send-event",
          events=_mtt("data", slot, ax, ay) + _mtt(phase, slot, ax, ay))


def pinch(q, cx, cy, r0, r1, steps=24, dt=0.03, angle=0.0, settle=0.3):
    """Two-finger pinch about (cx, cy), from separation 2*r0 to 2*r1 pixels.

    Both contacts are placed and committed before any motion: iPhone OS
    decides a gesture is a pinch from two contacts existing at once, and a
    first frame carrying only one finger reads as a drag that a second
    finger joined later, which is a different recogniser.
    """
    def pos(r):
        dx, dy = math.cos(angle) * r, math.sin(angle) * r
        return (cx - dx, cy - dy), (cx + dx, cy + dy)

    (ax, ay), (bx, by) = pos(r0)
    finger(q, 0, ax, ay, "begin")
    finger(q, 1, bx, by, "begin")
    time.sleep(settle)
    for i in range(1, steps + 1):
        r = r0 + (r1 - r0) * i / steps
        (ax, ay), (bx, by) = pos(r)
        finger(q, 0, ax, ay, "update")
        finger(q, 1, bx, by, "update")
        time.sleep(dt)
    time.sleep(settle)
    finger(q, 0, ax, ay, "end")
    finger(q, 1, bx, by, "end")


# ---------------------------------------------------------------------------
# Screenshots and kernel RAM.
# ---------------------------------------------------------------------------


def shot(q, path):
    """screendump + normalise. Returns (path, max_sample, nonzero_fraction).

    The panel scales every pixel by the guest-programmed backlight, so a raw
    dump of a dim screen is faithful and illegible; normalize() rescales it.
    """
    ppm = path + ".ppm"
    q.cmd("screendump", filename=ppm)
    for _ in range(40):
        if os.path.exists(ppm) and os.path.getsize(ppm) > 1000:
            break
        time.sleep(0.1)
    return normalize(ppm, path)


def normalize(ppm, png):
    with open(ppm, "rb") as f:
        data = f.read()
    # P6\n<w> <h>\n<maxval>\n<rgb...>
    parts, idx = [], 0
    while len(parts) < 4:
        while data[idx:idx + 1].isspace():
            idx += 1
        if data[idx:idx + 1] == b"#":
            while data[idx:idx + 1] != b"\n":
                idx += 1
            continue
        start = idx
        while not data[idx:idx + 1].isspace():
            idx += 1
        parts.append(data[start:idx])
    idx += 1
    w, h = int(parts[1]), int(parts[2])
    pix = bytearray(data[idx:idx + w * h * 3])
    hi = max(pix) if pix else 0
    if 0 < hi < 255:
        scale = 255.0 / hi
        pix = bytearray(min(255, int(v * scale)) for v in pix)
    out = b"P6\n%d %d\n255\n" % (w, h) + bytes(pix)
    tmp = png + ".norm.ppm"
    with open(tmp, "wb") as f:
        f.write(out)
    subprocess.run(["sips", "-s", "format", "png", tmp, "--out", png],
                   check=True, capture_output=True)
    os.remove(tmp)
    nz = sum(1 for v in pix if v) / max(1, len(pix))
    return png, hi, nz


def pmemsave(q, path, base=RAM_BASE, size=RAM_SIZE):
    """Dump guest RAM to `path` (for klog.py's msgbuf search)."""
    q.cmd("pmemsave", val=base, size=size, filename=path)


# ---------------------------------------------------------------------------
# CLI - contrib/ipod-touch-qmp.py is a thin wrapper around this.
# ---------------------------------------------------------------------------

USAGE = """\
itqmp.py <port-or-target> <action> [args...]

  shot <out.ppm>
  tap <x> <y>                       # 0..319 x, 0..479 y
  swipe <x1> <y1> <x2> <y2>
  button <home|power|voldown|volup>
  key <qcode>                       # raw key, reaches the guest as text
  finger <slot> <x> <y> <begin|update|end>
  pinch <cx> <cy> <r0> <r1>
  agent <operation> [args]          # binary body from stdin for exec/put
  cmd <qmp-command> [k=v ...]       # e.g. cmd system_powerdown

<port-or-target> is a TCP port, "host:port", or a unix socket path.
"""


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if len(argv) < 2:
        sys.stderr.write(USAGE)
        sys.exit("need a target and an action")
    target, action, rest = argv[0], argv[1], argv[2:]
    if action == "button" and (not rest or rest[0] not in BUTTONS):
        sys.exit("unknown button %r; try one of: %s"
                 % (rest[0] if rest else None, ", ".join(sorted(BUTTONS))))

    q = QMP(int(target) if target.isdigit() else target)
    if action == "agent":
        if not rest: sys.exit("agent operation required")
        body = sys.stdin.buffer.read() if rest[0] in ("exec", "put") and not sys.stdin.isatty() else b""
        status, output = agent(q, rest[0], " ".join(rest[1:]), body)
        sys.stdout.buffer.write(output)
        q.close()
        return 0 if status == 0 else 1
    elif action == "shot":
        print(shot(q, rest[0]))
    elif action == "tap":
        tap(q, int(rest[0]), int(rest[1]))
        print("tapped")
    elif action == "swipe":
        swipe(q, *(int(a) for a in rest[:4]))
        print("swiped")
    elif action == "button":
        button(q, rest[0])
        print("%s pressed" % rest[0])
    elif action == "key":
        key(q, rest[0])
        print("key sent")
    elif action == "finger":
        finger(q, int(rest[0]), int(rest[1]), int(rest[2]), rest[3])
        print("finger sent")
    elif action == "pinch":
        pinch(q, *(float(a) for a in rest[:4]))
        print("pinched")
    elif action == "cmd":
        args = {}
        for kv in rest[1:]:
            k, _, v = kv.partition("=")
            args[k] = v
        print(q.cmd(rest[0], **args))
    else:
        # Unknown action must fail loudly: a typo'd action otherwise silently
        # exits 0 while doing nothing, which has cost a full boot cycle to
        # debug before.
        sys.stderr.write(USAGE)
        sys.exit("unknown action %r" % action)


if __name__ == "__main__":
    sys.exit(main())
