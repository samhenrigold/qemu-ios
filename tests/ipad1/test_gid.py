#!/usr/bin/env python3
"""Catalog key extraction: production/development KBAGs and malformed input."""
import io
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'imgtools'))
from ipad1_gid import gid_blobs


def img3(prod, dev, bits=256):
    def tag(state, data):
        payload = struct.pack('<II', state, bits) + data
        return struct.pack('<4sII', b'GABK', len(payload) + 12, len(payload)) + payload
    tags = tag(1, prod) + tag(2, dev)
    return struct.pack('<4sIIII', b'3gmI', 20 + len(tags), len(tags), 0, 0) + tags


class CatalogKeys(unittest.TestCase):
    def extract(self, images, key='22' * 32):
        with tempfile.TemporaryDirectory() as td:
            keys = Path(td) / 'keys'
            keys.write_text('\nKernelcache\nkernelcache.release.k48\nIV: ' + '11' * 16 + '\nKey: ' + key + '\n')
            stream = io.BytesIO()
            with zipfile.ZipFile(stream, 'w') as z:
                for name, data in images.items():
                    z.writestr(name, data)
            with zipfile.ZipFile(stream) as z:
                return gid_blobs(z, keys)

    def test_production_precedes_development(self):
        data, names = self.extract({'kernelcache.release.k48': img3(b'P' * 48, b'D' * 48)})
        self.assertEqual(data, b'P' * 48 + bytes.fromhex('11' * 16 + '22' * 32) + b'D' * 48 + bytes.fromhex('11' * 16 + '22' * 32))
        self.assertEqual(names, ['kernelcache.release.k48'])

    def test_short_key_rejected(self):
        with self.assertRaises(ValueError):
            self.extract({'kernelcache.release.k48': img3(b'P' * 48, b'D' * 48)}, '22' * 16)

    def test_truncated_image_rejected(self):
        with self.assertRaises(ValueError):
            self.extract({'kernelcache.release.k48': img3(b'P' * 48, b'D' * 48)[:-1]})

    def test_wrong_catalog_rejected(self):
        with self.assertRaises(ValueError):
            self.extract({'unknown.img3': img3(b'P' * 48, b'D' * 48)})


class SyntheticIdentity(unittest.TestCase):
    def test_rom_ecid_and_revision_match_identity(self):
        from ipad1_kboot import synth_identity
        for seed in ('first', 'second', 'third'):
            ident = synth_identity(seed)
            w2, w3 = (int(x, 16) for x in ident['die-id'])
            ecid = (((((w2 & 0x1fffff) << 5 | ((w2 >> 21) & 31)) << 8 |
                       ((w3 >> 2) & 255)) << 6 | ((w2 >> 26) & 63)) << 2 | (w3 & 3))
            self.assertEqual(ecid, int(ident['unique-chip-id'], 16))
            self.assertEqual(((w3 >> 9) & 0x70) | ((w3 >> 10) & 7), 0x11)


if __name__ == '__main__':
    unittest.main()
