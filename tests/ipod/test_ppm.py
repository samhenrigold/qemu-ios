#!/usr/bin/env python3
"""All screenshot consumers share a bounded parser, including partial boot dumps."""
import subprocess
import sys
import tempfile
from pathlib import Path
root = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(root/'imgtools'))
import itqmp
import record
sys.path.insert(0, str(root/'tests/ipod'))
import regress
assert record.read_ppm is regress.read_ppm is itqmp.read_ppm
with tempfile.TemporaryDirectory() as temp:
    path = Path(temp)/'frame.ppm'
    for header in [b'P6\n1 1\n255\n', b'P6\r\n# comment\r\n1\t1\r\n255\r\n']:
        path.write_bytes(header + b'\n #')
        assert itqmp.read_ppm(path) == (1,1,bytearray(b'\n #'))
    path.write_bytes(b'P6\n1 1\n1\n\x01\x00\x01')
    assert itqmp.read_ppm(path)[2] == b'\x01\x00\x01'
    for data in [b'',b'P6',b'P6\n# never ends',b'P6\n1 1\n255',b'P6\n1 1\n255\n\x00',
                 b'P3\n1 1\n255\nabc',b'P6\n0 1\n255\n',b'P6\n1 1\n65535\nabcdef',b'P6\n1 1\n255\nextra']:
        path.write_bytes(data)
        # Bound the old infinite-loop failure without hanging this test itself.
        result = subprocess.run([sys.executable,'-c',
            'import sys;sys.path.insert(0,sys.argv[1]);import itqmp;itqmp.read_ppm(sys.argv[2])',
            str(root/'imgtools'),str(path)],capture_output=True,timeout=2)
        assert result.returncode and b'ValueError' in result.stderr,(data,result.stderr)
print('PASS: shared PPM parser, pixel delimiters, dim samples and bounded malformed-input rejection')
