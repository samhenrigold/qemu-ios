#!/usr/bin/env python3
"""
Dump the emulated device's complete USB descriptor set over the tcp_usb
transport.

Answers one question: does iPhone OS 2.1.1 expose a USB ethernet/tethering
function on this hardware? A CDC ECM/NCM or RNDIS function would show up here
as an interface with bInterfaceClass 0x02 (communications) or 0x0a (CDC data).

Usage: ipod-touch-usbdesc.py [port] [boot_delay_seconds]
"""
import socket
import struct
import sys
import time

HDR = struct.Struct("<BBBh")

F_SETUP = 1 << 0
F_RESET = 1 << 1
F_ENUMDONE = 1 << 2
F_HELLO = 1 << 3

USB_DIR_IN = 0x80

RET = {-1: "NODEV", -2: "NAK", -3: "STALL", -4: "BABBLE", -5: "IOERROR"}

CLASS_NAMES = {
    0x00: "per-interface",
    0x01: "audio",
    0x02: "communications (CDC control)",
    0x03: "HID",
    0x06: "still image (PTP)",
    0x08: "mass storage",
    0x0a: "CDC data",
    0x0e: "video",
    0xe0: "wireless controller",
    0xef: "miscellaneous",
    0xff: "vendor specific",
}


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("device closed the connection")
        buf += chunk
    return buf


def transact(sock, ep, flags=0, length=0, payload=b""):
    sock.sendall(HDR.pack(0, ep, flags, length))
    if not (ep & USB_DIR_IN) and payload:
        sock.sendall(payload)
    addr, r_ep, r_flags, r_len = HDR.unpack(recv_exact(sock, HDR.size))
    data = b""
    if (r_ep & USB_DIR_IN) and r_len > 0:
        data = recv_exact(sock, r_len)
    return addr, r_len, data


def describe(n):
    return RET.get(n, f"err{n}") if n < 0 else f"{n} bytes"


CTRL_TIMEOUT = 5.0
STATUS_TIMEOUT = 0.3
IN_IDLE_NAKS = 20


def status_stage(sock, in_dir):
    """Best effort: the device does not track control phases, so the opposite
    direction may NAK forever. Time out quietly."""
    deadline = time.time() + STATUS_TIMEOUT
    while time.time() < deadline:
        _, r, _ = transact(sock, USB_DIR_IN if in_dir else 0, length=0)
        if r >= 0 or r in (-1, -5):
            return
        time.sleep(0.001)


def control_in(sock, bRequest, wValue, wIndex, wLength, bmRequestType=0x80):
    """SETUP (retried until accepted), then poll EP0 IN. Returns bytes or None."""
    setup = struct.pack("<BBHHH", bmRequestType, bRequest, wValue, wIndex, wLength)
    deadline = time.time() + CTRL_TIMEOUT
    while True:
        _, r, _ = transact(sock, 0, flags=F_SETUP, length=len(setup), payload=setup)
        if r >= 0:
            break
        if r in (-1, -5) or time.time() > deadline:
            print(f"      SETUP never accepted for request {bRequest:#04x} "
                  f"value {wValue:#06x} ({describe(r)})")
            return None
        time.sleep(0.001)

    out = b""
    naks = 0
    deadline = time.time() + CTRL_TIMEOUT
    while len(out) < wLength:
        _, r, data = transact(sock, USB_DIR_IN | 0, length=wLength - len(out))
        if r > 0:
            out += data
            naks = 0
            continue
        if r == 0:
            break
        if r in (-1, -3, -5):
            if out:
                break
            print(f"      {describe(r)} on request {bRequest:#04x} "
                  f"value {wValue:#06x}")
            return None
        naks += 1
        if out and naks > IN_IDLE_NAKS:
            break
        if time.time() > deadline:
            if out:
                break
            print(f"      timed out (still NAKing) on request {bRequest:#04x} "
                  f"value {wValue:#06x}")
            return None
        time.sleep(0.001)

    status_stage(sock, False)
    return out


def get_string(sock, index, langid=0x0409):
    if index == 0:
        return ""
    raw = control_in(sock, 0x06, 0x0300 | index, langid, 255)
    if not raw or len(raw) < 2:
        return f"<index {index}: unreadable>"
    try:
        return raw[2:raw[0]].decode("utf-16-le", errors="replace")
    except Exception:
        return f"<index {index}: undecodable>"


def parse_config(sock, raw):
    """Walk the standard descriptor chain of one configuration."""
    i = 0
    while i + 2 <= len(raw):
        blen = raw[i]
        btype = raw[i + 1]
        if blen == 0:
            print(f"    !! zero-length descriptor at offset {i}, stopping")
            break
        d = raw[i:i + blen]
        if btype == 0x02 and blen >= 9:
            (_, _, wTotal, nIf, cfgval, iCfg, attrs, maxpwr) = struct.unpack(
                "<BBHBBBBB", d[:9])
            name = get_string(sock, iCfg)
            print(f"  CONFIGURATION {cfgval}: {nIf} interface(s), "
                  f"attrs {attrs:#04x}, {maxpwr * 2} mA  \"{name}\"")
        elif btype == 0x04 and blen >= 9:
            (_, _, num, alt, nEp, cls, sub, proto, iIf) = struct.unpack(
                "<BBBBBBBBB", d[:9])
            name = get_string(sock, iIf)
            cname = CLASS_NAMES.get(cls, "unknown")
            print(f"    INTERFACE {num} alt {alt}: class {cls:#04x} ({cname}) "
                  f"subclass {sub:#04x} protocol {proto:#04x}, "
                  f"{nEp} endpoint(s)  \"{name}\"")
            if cls in (0x02, 0x0a):
                print("      *** COMMUNICATIONS/CDC CLASS - possible ethernet "
                      "function, inspect functional descriptors ***")
        elif btype == 0x05 and blen >= 7:
            (_, _, addr, attrs, mps, interval) = struct.unpack("<BBBBHB", d[:7])
            direction = "IN" if addr & 0x80 else "OUT"
            xfer = ["control", "isochronous", "bulk", "interrupt"][attrs & 3]
            print(f"      ENDPOINT {addr:#04x} {direction} {xfer} "
                  f"mps {mps} interval {interval}")
        elif btype == 0x0b and blen >= 8:
            (_, _, first, cnt, cls, sub, proto, iFn) = struct.unpack(
                "<BBBBBBBB", d[:8])
            print(f"    IAD: interfaces {first}..{first + cnt - 1} "
                  f"class {cls:#04x} subclass {sub:#04x} protocol {proto:#04x}")
        elif btype == 0x24:
            sub = d[2] if blen >= 3 else 0xff
            extra = ""
            if sub == 0x0f:
                extra = "  <-- CDC ETHERNET NETWORKING functional descriptor"
            print(f"    CS_INTERFACE subtype {sub:#04x}: "
                  f"{' '.join(f'{b:02x}' for b in d)}{extra}")
        else:
            print(f"    descriptor type {btype:#04x} len {blen}: "
                  f"{' '.join(f'{b:02x}' for b in d)}")
        i += blen


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 1330
    delay = int(sys.argv[2]) if len(sys.argv) > 2 else 100

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    print(f"[host] listening on 127.0.0.1:{port}", flush=True)

    conn, peer = srv.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"[host] device connected from {peer}", flush=True)

    _, r, data = transact(conn, USB_DIR_IN | 0x7f, flags=F_HELLO, length=12)
    if r >= 12:
        magic, ver, _, maxtx = struct.unpack("<IHHI", data[:12])
        print(f"[host] hello: magic {magic:#010x} version {ver} "
              f"max_transaction {maxtx}", flush=True)
        if magic != 0x42535554:
            print("[host] !!! bad magic, aborting")
            return
    else:
        print(f"[host] hello failed: {describe(r)}", flush=True)

    print(f"[host] waiting {delay}s for the guest to program the USB core",
          flush=True)
    time.sleep(delay)

    print("[host] --> RESET", flush=True)
    transact(conn, 0, flags=F_RESET)
    time.sleep(2)
    print("[host] --> ENUMDONE", flush=True)
    transact(conn, 0, flags=F_ENUMDONE)
    time.sleep(2)

    dev = control_in(conn, 0x06, 0x0100, 0, 18)
    if not dev or len(dev) < 18:
        print("[host] !!! no device descriptor, cannot continue", flush=True)
        return
    (_, _, bcdUSB, cls, sub, proto, mps0, vid, pid, bcdDev,
     iMfr, iProd, iSer, nCfg) = struct.unpack("<BBHBBBBHHHBBBB", dev[:18])
    print(f"[host] DEVICE: USB {bcdUSB:#06x} class {cls:#04x} subclass "
          f"{sub:#04x} protocol {proto:#04x} mps0 {mps0}", flush=True)
    print(f"        idVendor {vid:#06x} idProduct {pid:#06x} "
          f"bcdDevice {bcdDev:#06x} bNumConfigurations {nCfg}", flush=True)
    print(f"        manufacturer: {get_string(conn, iMfr)!r}", flush=True)
    print(f"        product:      {get_string(conn, iProd)!r}", flush=True)
    print(f"        serial:       {get_string(conn, iSer)!r}", flush=True)

    for idx in range(nCfg):
        head = control_in(conn, 0x06, 0x0200 | idx, 0, 9)
        if not head or len(head) < 9:
            print(f"[host] config index {idx}: no descriptor", flush=True)
            continue
        wTotal = struct.unpack_from("<H", head, 2)[0]
        full = control_in(conn, 0x06, 0x0200 | idx, 0, wTotal)
        if not full:
            print(f"[host] config index {idx}: short read", flush=True)
            continue
        print(f"[host] --- configuration index {idx} "
              f"({len(full)}/{wTotal} bytes) ---", flush=True)
        parse_config(conn, full)

    print("[host] done", flush=True)
    conn.close()
    srv.close()


if __name__ == "__main__":
    main()
