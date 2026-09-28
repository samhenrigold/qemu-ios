"""Host side of QEMU's transfer-oriented TCP USB bridge (protocol v1)."""
import socket
import struct
import time

HEADER = struct.Struct('<BBBh')


class USBError(RuntimeError):
    pass


class TCPUSB:
    def __init__(self, connection):
        self.connection = connection
        self.connection.settimeout(5)
        self.address = 0

    def read(self, size):
        data = bytearray()
        while len(data) < size:
            part = self.connection.recv(size - len(data))
            if not part:
                raise EOFError('QEMU USB disconnected')
            data += part
        return bytes(data)

    def packet(self, ep, size=0, data=b'', flags=0):
        if not 0 <= size <= 32767 or (not ep & 128 and len(data) != size):
            raise ValueError('invalid TCP USB transfer size')
        self.connection.sendall(HEADER.pack(self.address, ep, flags, size) + (data if not ep & 128 else b''))
        self.address, reply_ep, reply_flags, length = HEADER.unpack(self.read(HEADER.size))
        if reply_ep != ep:
            raise USBError('unexpected endpoint in reply')
        if length > size:
            raise USBError('oversized reply')
        return length, self.read(length) if ep & 128 and length > 0 else b''

    def transfer(self, ep, size=0, data=b'', flags=0, timeout=10):
        end = time.monotonic() + timeout
        while True:
            count, reply = self.packet(ep, size, data, flags)
            if count >= 0:
                return reply if ep & 128 else count
            if count != -2:
                raise USBError(f'endpoint {ep:#x}: USB status {count}')
            if time.monotonic() >= end:
                raise TimeoutError(f'endpoint {ep:#x}: NAK timeout')
            time.sleep(.001)

    def control(self, kind, request, value=0, index=0, data=b'', length=0, timeout=10):
        incoming = bool(kind & 128)
        length = length if incoming else len(data)
        setup = struct.pack('<BBHHH', kind, request, value, index, length)
        self.transfer(0, 8, setup, flags=1, timeout=timeout)
        result = bytearray()
        if incoming:
            while len(result) < length:
                part = self.transfer(128, min(64, length - len(result)), timeout=timeout)
                result += part
                if len(part) < 64:
                    break
            self.transfer(0, timeout=timeout)
            return bytes(result)
        for off in range(0, length, 64):
            part = data[off:off + 64]
            sent = self.transfer(0, len(part), part, timeout=timeout)
            if sent != len(part):
                raise USBError('short control OUT transfer')
        self.transfer(128, timeout=timeout)
        return length

    def enumerate(self):
        self.packet(0, flags=2)
        time.sleep(.05)
        self.packet(0, flags=4)
        time.sleep(.05)
        desc = self.control(128, 6, 0x100, length=18)
        if len(desc) != 18:
            raise USBError('short device descriptor')
        self.control(0, 5, 1)
        config = self.control(128, 6, 0x200, length=9)
        config = self.control(128, 6, 0x200, length=struct.unpack_from('<H', config, 2)[0])
        self.control(0, 9, config[5])
        serial = self.control(128, 6, 0x300 | desc[16], 0x409, length=255)
        return desc, config, serial[2:].decode('utf-16-le')
