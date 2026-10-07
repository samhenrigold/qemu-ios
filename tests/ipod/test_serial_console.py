#!/usr/bin/env python3
"""An echoed iBoot argument is not proof that the kernel console works."""
from pathlib import Path
import tempfile
from types import SimpleNamespace
from unittest.mock import patch
import regress as R

with tempfile.TemporaryDirectory() as tmp, patch.object(R, 'log'):
    path=Path(tmp)/'serial.log';dev=SimpleNamespace(serial=str(path))
    for data,expected in [
        (b'gBootArgs.commandLine = [serial=3 debug=0x8]',False),
        (b'AppleS5L8720XFMSS::start: init',False),
        (b'BSD root: disk0s1, major 14, minor 1',False),
        (b'\0\rAppleS5L8720XFMSS::start: init\r\nBSD root: disk0s1, major 14',True),
    ]:
        path.write_bytes(data)
        assert R.check_serial_console(dev,R.Result('serial-console')) is expected
    path.unlink()
    assert not R.check_serial_console(dev,R.Result('serial-console'))
print('PASS: kernel console requires kernel output beyond iBoot arguments')
