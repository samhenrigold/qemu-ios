#!/usr/bin/env python3
"""NOR packing preserves signed bytes and applies wrapping per boot stage."""
from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'imgtools'))
import build_nor as nor


class WrappingTests(unittest.TestCase):
    def test_stage_selection(self):
        ident = dict(zip(('model-number', 'region-info', 'serial-number',
                          'battery-serial', 'bt-mac', 'wifi-mac'),
                         ('MB528', 'LL/A', 'TEST', 'TEST',
                          '02:00:00:00:00:01', '02:00:00:00:00:02')))
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            images = {}
            for tag in ('illb', 'ibot', 'dtre'):
                data = struct.pack('<4sII', b'ATAD', 28, 16) + bytes(range(16))
                shsh = struct.pack('<4sII', b'HSHS', 140, 128) + bytes(range(128))
                body = data + shsh
                image = struct.pack('<4sIII4s', b'3gmI', len(body) + 20,
                                    len(body), len(data), tag[::-1].encode()) + body
                images[tag] = image
                (p / (tag + '.img3')).write_bytes(image)
            for selected in (None, ['ibot'], []):
                out = p / 'nor.bin'
                nor.build(None, str(p), str(out), list(images), verbose=False,
                          base=nor.synth_base(ident), wrap_types=selected)
                blob = out.read_bytes()
                offset = 0x8000
                for tag, original in images.items():
                    size = struct.unpack_from('<I', blob, offset + 4)[0]
                    actual = blob[offset:offset + size]
                    # Neither the signed header suffix nor DATA is rewritten.
                    self.assertEqual(actual[8:48], original[8:48])
                    expected = original[60:188]
                    wrapped = selected is None or tag in selected
                    if wrapped:
                        expected = nor._aes_cbc(nor.shsh_wrap_key(nor.DEFAULT_UID_KEY), expected, True)
                    self.assertEqual(actual[60:188], expected)
                    self.assertEqual(size % 64, 0)
                    offset += size
            with self.assertRaises(SystemExit):
                nor.build(None, str(p), str(out), list(images), verbose=False,
                          base=nor.synth_base(ident), wrap_types=['missing'])


if __name__ == '__main__':
    unittest.main()
