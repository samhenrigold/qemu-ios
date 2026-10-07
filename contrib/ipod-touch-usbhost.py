#!/usr/bin/env python3
"""
Throwaway host bridge for the tcp_usb transport - milestone M1.

QEMU is the client and dials us, so we listen. The protocol is strictly
host-driven request/response: we send a 5-byte header (plus payload for OUT),
the device answers with a header (plus payload for IN). A negative response
length is a USB_RET_* code (NAK/STALL) and means "retry", which is the whole
flow-control mechanism.

Goal: drive reset -> enumdone -> SETUP GET_DESCRIPTOR(DEVICE) and read back the
18-byte device descriptor.
"""
import socket
import struct
import sys
import time

HDR = struct.Struct("<BBBh")  # addr, ep, flags, length (packed, 5 bytes)

F_SETUP = 1 << 0
F_RESET = 1 << 1
F_ENUMDONE = 1 << 2

USB_DIR_IN = 0x80

# QEMU returns these as negative lengths.
USB_RET_CODES = {-1: "NODEV", -2: "NAK", -3: "STALL", -4: "BABBLE", -5: "IOERROR"}


def recv_exact(sock, n):
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("device closed the connection")
        buf += chunk
    return buf


def transact(sock, ep, flags=0, length=0, payload=b""):
    """One request/response round trip. Returns (resp_addr, resp_len, data)."""
    sock.sendall(HDR.pack(0, ep, flags, length))
    if not (ep & USB_DIR_IN) and payload:
        sock.sendall(payload)

    addr, r_ep, r_flags, r_len = HDR.unpack(recv_exact(sock, HDR.size))
    data = b""
    if (r_ep & USB_DIR_IN) and r_len > 0:
        data = recv_exact(sock, r_len)
    return addr, r_len, data


def describe(r_len):
    if r_len < 0:
        return USB_RET_CODES.get(r_len, f"err{r_len}")
    return f"{r_len} bytes"


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 1235

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", port))
    srv.listen(1)
    print(f"[host] listening on 127.0.0.1:{port}", flush=True)

    conn, addr = srv.accept()
    conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    print(f"[host] device connected from {addr}", flush=True)

    # Give the guest driver time to finish programming the core. It parks with
    # GINTMSK = RESET|ENUMDONE, so there is no point signalling before that.
    time.sleep(int(sys.argv[2]) if len(sys.argv) > 2 else 90)

    print("[host] --> RESET", flush=True)
    _, r, _ = transact(conn, 0, flags=F_RESET)
    print(f"[host] <-- {describe(r)}", flush=True)
    time.sleep(2)

    print("[host] --> ENUMDONE", flush=True)
    _, r, _ = transact(conn, 0, flags=F_ENUMDONE)
    print(f"[host] <-- {describe(r)}", flush=True)
    time.sleep(2)

    # GET_DESCRIPTOR(DEVICE, index 0), wLength 18
    setup = bytes([0x80, 0x06, 0x00, 0x01, 0x00, 0x00, 0x12, 0x00])
    print("[host] --> SETUP GET_DESCRIPTOR(DEVICE)", flush=True)
    a, r, _ = transact(conn, 0, flags=F_SETUP, length=len(setup), payload=setup)
    print(f"[host] <-- {describe(r)} (device addr {a})", flush=True)

    # Poll EP0 IN. Everything NAKs until the guest sets USB_EPCON_ENABLE, so the
    # host must keep asking rather than treat a NAK as failure.
    print("[host] --> polling EP0 IN for the descriptor", flush=True)
    for attempt in range(60):
        a, r, data = transact(conn, USB_DIR_IN | 0, length=18)
        if r > 0:
            print(f"[host] <-- GOT {r} bytes from EP0 IN (device addr {a}):", flush=True)
            print("[host]     " + " ".join(f"{b:02x}" for b in data), flush=True)
            if len(data) >= 2 and data[1] == 0x01:
                print("[host] *** bDescriptorType == 0x01 (DEVICE) - M1 PASS ***", flush=True)
                if len(data) >= 12:
                    vid, pid = struct.unpack_from("<HH", data, 8)
                    print(f"[host]     idVendor={vid:#06x} idProduct={pid:#06x}", flush=True)
            else:
                print("[host] !!! data returned but it is not a DEVICE descriptor", flush=True)
            break
        if attempt % 10 == 0:
            print(f"[host]     attempt {attempt}: {describe(r)}", flush=True)
        time.sleep(0.5)
    else:
        print("[host] !!! EP0 IN never returned data (still NAKing) - M1 FAIL", flush=True)

    conn.close()
    srv.close()


if __name__ == "__main__":
    main()
