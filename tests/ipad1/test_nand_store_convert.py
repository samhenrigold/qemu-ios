#!/usr/bin/env python3
"""Legacy zero ambiguity and cold physical data semantics of offline conversion."""
import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'imgtools'))
from nand_store_convert import convert


class Conversion(unittest.TestCase):
    def setUp(self):
        self.scratch = tempfile.TemporaryDirectory()
        self.root = Path(self.scratch.name)
        self.source = self.root / 'legacy'
        self.source.mkdir()
        self.geometry = dict(page_bytes=4, spare_bytes=10, pages_per_block=1,
                             blocks_per_ce=3, ce_per_bus=1, buses=2, chip_id='0xB614D5AD')
        (self.source / 'geometry.json').write_text(json.dumps(self.geometry))
        # Page0 ambiguous zeros, page1 actual FF, page2 physical mixed data.
        self.original = bytes(14) + b'\xff' * 14 + b'\x00\x55\xaa\xff' + b'\xfe' * 10
        for bus in range(2):
            (self.source / f'bus{bus}-ce0.pages').write_bytes(self.original)

    def tearDown(self):
        self.scratch.cleanup()

    def test_ambiguity_rejects_without_publishing(self):
        destination = self.root / 'converted'
        with self.assertRaisesRegex(ValueError, 'ambiguous'):
            convert(self.source, destination)
        self.assertFalse(destination.exists())
        self.assertEqual(list(self.root.iterdir()), [self.source])

    def test_explicit_policies_and_unchanged_source(self):
        for policy, expected in [('erased', b'\xff' * 14), ('data', bytes(14))]:
            destination = self.root / policy
            report = convert(self.source, destination, policy)
            decoded = bytes(n ^ 255 for n in (destination / 'bus0-ce0.pages').read_bytes())
            self.assertEqual(decoded, expected + self.original[14:])
            self.assertEqual(report['zero_pages'], 2)
            self.assertEqual(json.loads((destination / 'geometry.json').read_text())['storage_format'], 'nand-xor-ff-v2')
        self.assertEqual((self.source / 'bus0-ce0.pages').read_bytes(), self.original)

    def test_truncation_and_existing_output_rejected(self):
        destination = self.root / 'converted'
        destination.mkdir()
        with self.assertRaisesRegex(ValueError, 'already exists'):
            convert(self.source, destination, 'erased')
        destination.rmdir()
        (self.source / 'bus1-ce0.pages').write_bytes(b'x')
        with self.assertRaisesRegex(ValueError, 'size/type'):
            convert(self.source, destination, 'erased')
        self.assertFalse(destination.exists())


if __name__ == '__main__':
    unittest.main()
