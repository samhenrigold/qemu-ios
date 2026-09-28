#!/usr/bin/env python3
"""Expose QEMU TCP USB to the optional libirecovery transport adapter.

Only the emulator socket is used. This process never opens physical USB devices.
"""
import argparse
import os
import socket
import socketserver
import struct
import threading
import time
from tcp_usb import TCPUSB, USBError

MAX_REQUEST = 1 << 20


def receive(connection, size):
    data = bytearray()
    while len(data) < size:
        part = connection.recv(size - len(data))
        if not part:
            raise EOFError
        data += part
    return bytes(data)


class Bridge:
    def __init__(self, listener, mux_addr=None):
        self.listener = listener
        self.mux_addr = mux_addr
        self.handed_off = False
        self.usb = None
        self.info = None
        self.lock = threading.Lock()

    def handoff(self):
        """Let usbmuxd-qemu own the live guest link after restore-kernel boot."""
        import selectors
        host, port = self.mux_addr.rsplit(':', 1)
        mux = socket.create_connection((host, int(port)))
        guest = self.usb.connection
        guest.settimeout(None)
        self.handed_off = True

        def relay():
            try:
                with selectors.DefaultSelector() as selector:
                    selector.register(guest, selectors.EVENT_READ, mux)
                    selector.register(mux, selectors.EVENT_READ, guest)
                    while True:
                        for key, _ in selector.select():
                            data = key.fileobj.recv(65536)
                            if not data:
                                return
                            key.data.sendall(data)
            finally:
                guest.close()
                mux.close()
        threading.Thread(target=relay, daemon=True).start()
        print(f'USB handed to usbmuxd-qemu at {self.mux_addr}', flush=True)

    def connect(self):
        if self.handed_off:
            raise ConnectionError('restore kernel now belongs to usbmuxd')
        if self.usb:
            return
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            connection, _ = self.listener.accept()
            self.usb = TCPUSB(connection)
            time.sleep(.1)
            try:
                desc, config, serial = self.usb.enumerate()
                mode = struct.unpack_from('<H', desc, 10)[0]
                self.info = struct.pack('<I', mode) + serial.encode() + b'\0'
                print(f'USB {mode:04x}: {serial}', flush=True)
                return
            except (EOFError, ConnectionError):
                self.usb.connection.close()
                self.usb = None
        raise TimeoutError('no stable QEMU USB connection')

    def request(self, op, payload):
        with self.lock:
            if self.handed_off:
                return -1, b''
            self.connect()
            try:
                if op == 1:
                    # Poll the guest descriptor, not a fabricated mode. Firmware
                    # transitions may change PID without closing the TCP socket.
                    desc = self.usb.control(128, 6, 0x100, length=18, timeout=1)
                    mode = struct.unpack_from('<H', desc, 10)[0]
                    if self.mux_addr and 0x1290 <= mode <= 0x12af:
                        self.handoff()
                        return -1, b''
                    if mode != struct.unpack_from('<I', self.info)[0]:
                        desc, config, text = self.usb.enumerate()
                        self.info = struct.pack('<I', mode) + text.encode() + b'\0'
                        print(f'USB transition {mode:04x}: {text}', flush=True)
                        return len(self.info), self.info
                    serial = self.usb.control(128, 6, 0x300 | desc[16], 0x409, length=255, timeout=1)
                    self.info = struct.pack('<I', struct.unpack_from('<H', desc, 10)[0]) + serial[2:].decode('utf-16-le').encode() + b'\0'
                    return len(self.info), self.info
                if op == 2:
                    kind, req, value, index, length, timeout = struct.unpack('<BBHHHI', payload[:12])
                    result = self.usb.control(kind, req, value, index, payload[12:], length, max(timeout / 1000, 1))
                    return (len(result), result) if isinstance(result, bytes) else (result, b'')
                if op == 3:
                    ep, length, timeout = struct.unpack('<III', payload[:12])
                    data = payload[12:]
                    if length > 32767:
                        # Recovery uploads are byte streams. 16 KiB is also a
                        # multiple of the 512-byte bulk maximum packet size.
                        if ep & 128:
                            length = 16384
                        else:
                            done = 0
                            while done < length:
                                chunk = data[done:done + 16384]
                                n = self.usb.transfer(ep, len(chunk), chunk, timeout=max(timeout / 1000, 1))
                                if n <= 0:
                                    raise USBError('short bulk OUT')
                                done += n
                            return done, b''
                    result = self.usb.transfer(ep, length, data, timeout=max(timeout / 1000, 1))
                    return (len(result), result) if isinstance(result, bytes) else (result, b'')
                if op == 4:
                    self.usb.packet(0, flags=2)
                    return 0, b''
                raise ValueError('unknown bridge operation')
            except (EOFError, ConnectionError):
                self.usb.connection.close()
                self.usb = None
                raise


class Handler(socketserver.BaseRequestHandler):
    def handle(self):
        try:
            op, size = struct.unpack('<II', receive(self.request, 8))
            if size > MAX_REQUEST:
                return
            payload = receive(self.request, size)
            result, data = self.server.bridge.request(op, payload)
        except (OSError, EOFError, USBError, ValueError, IndexError, struct.error) as error:
            print(f'USB request failed: {error}', flush=True)
            result, data = -1, b''
        self.request.sendall(struct.pack('<iI', result, len(data)) + data)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--socket', required=True)
    parser.add_argument('--port', type=int, default=23592)
    parser.add_argument('--usbmuxd', help='usbmuxd-qemu TCP USB listener for restore-kernel handoff (host:port)')
    a = parser.parse_args()
    with socket.socket() as listener:
        listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        listener.bind(('127.0.0.1', a.port))
        listener.listen()
        listener.settimeout(3)
        # Refuse to unlink somebody else's active socket.
        with socketserver.ThreadingUnixStreamServer(a.socket, Handler) as server:
            os.chmod(a.socket, 0o600)
            server.bridge = Bridge(listener, a.usbmuxd)
            print(f'QEMU USB port {a.port}; libirecovery socket {a.socket}', flush=True)
            try:
                server.serve_forever()
            finally:
                os.unlink(a.socket)


if __name__ == '__main__':
    main()
