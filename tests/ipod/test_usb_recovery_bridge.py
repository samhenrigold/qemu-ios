#!/usr/bin/env python3
"""Firmware USB resets must enumerate again without configuring the mux interface."""
import importlib
import struct
import sys
import threading
import unittest
from pathlib import Path
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'imgtools'))
bridge_module = importlib.import_module('usb_recovery_bridge')
TCPUSB = importlib.import_module('tcp_usb').TCPUSB


def descriptor(pid):
    result = bytearray(18)
    struct.pack_into('<H', result, 10, pid)
    result[16] = 1
    return bytes(result)


class FakeUSB:
    def __init__(self, pid, address=0):
        self.pid, self.address = pid, address
        self.calls = []
        self.connection = unittest.mock.Mock()

    def device_descriptor(self, reset=False, timeout=10):
        self.calls.append(('descriptor', reset, timeout))
        return descriptor(self.pid)

    def configure(self, desc):
        self.calls.append(('configure', self.pid))
        self.address = 1
        return desc, b'config', 'actual guest serial'

    def control(self, *args, **kwargs):
        self.calls.append(('serial',))
        return b'\x08\x03' + 'ECID'.encode('utf-16-le')


class BridgeTests(unittest.TestCase):
    def connected(self, pid, address=0):
        bridge = bridge_module.Bridge(None, '127.0.0.1:1234')
        bridge.usb = FakeUSB(pid, address)
        bridge.info = struct.pack('<I', 0x1281) + b'previous\0'
        def handoff():
            self.assertTrue(bridge.lock.locked())
            bridge.handed_off = True
            bridge.usb.calls.append(('handoff',))
        bridge.handoff = handoff
        return bridge

    def test_reset_with_unchanged_recovery_pid_reconfigures(self):
        bridge = self.connected(0x1281)
        count, data = bridge.request(1, b'')
        self.assertEqual(count, len(data))
        self.assertIn(b'actual guest serial', data)
        self.assertEqual(bridge.usb.calls, [('descriptor', True, 1), ('configure', 0x1281)])

    def test_kernel_handoff_precedes_configuration_and_has_one_owner(self):
        bridge = self.connected(0x1293)
        results = []
        threads = [threading.Thread(target=lambda: results.append(bridge.request(1, b'')))
                   for _ in range(2)]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        self.assertEqual(results, [(-1, b''), (-1, b'')])
        self.assertEqual(bridge.usb.calls, [('descriptor', True, 1), ('handoff',)])
        bridge.usb.connection.close.assert_not_called()

    def test_stable_recovery_poll_does_not_reset(self):
        bridge = self.connected(0x1281, address=1)
        bridge.request(1, b'')
        self.assertEqual(bridge.usb.calls, [('descriptor', False, 1), ('serial',)])

    def test_initial_kernel_connection_does_not_select_recovery_config(self):
        listener = unittest.mock.Mock()
        connection = unittest.mock.Mock()
        listener.accept.return_value = (connection, None)
        bridge = bridge_module.Bridge(listener, '127.0.0.1:1234')
        usb = FakeUSB(0x1293)
        def handoff():
            bridge.handed_off = True
            usb.calls.append(('handoff',))
        bridge.handoff = handoff
        with patch.object(bridge_module, 'TCPUSB', return_value=usb), patch.object(bridge_module.time, 'sleep'):
            with self.assertRaises(ConnectionError):
                bridge.connect()
        self.assertEqual(usb.calls, [('descriptor', True, 10), ('handoff',)])
        usb.connection.close.assert_not_called()

    def test_descriptor_probe_uses_bus_events_without_configuration(self):
        usb = TCPUSB(unittest.mock.Mock())
        usb.packet = unittest.mock.Mock()
        usb.control = unittest.mock.Mock(return_value=descriptor(0x1293))
        with patch('tcp_usb.time.sleep'):
            self.assertEqual(usb.device_descriptor(reset=True), descriptor(0x1293))
        self.assertEqual(usb.packet.call_args_list,
                         [unittest.mock.call(0, flags=2), unittest.mock.call(0, flags=4)])
        usb.control.assert_called_once_with(128, 6, 0x100, length=18, timeout=10)
        usb.control.return_value = b'bad'
        with self.assertRaises(bridge_module.USBError):
            usb.device_descriptor()


if __name__ == '__main__':
    unittest.main()
