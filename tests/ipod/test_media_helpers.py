#!/usr/bin/env python3
"""Offline checks for media identity queries and photo receipt reconciliation.

The photo harness compiles the production receipt-handling code and stops
before loading Foundation or submitting a save. All files are local fixtures.
"""
import ast
from pathlib import Path
import re
import shutil
import sqlite3
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PhotoReceipts(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.scratch = tempfile.TemporaryDirectory(prefix="it-photo-receipts-")
        cls.addClassCleanup(cls.scratch.cleanup)
        cls.work = Path(cls.scratch.name)
        cls.media = cls.work / "Media"
        source = (ROOT / "contrib/it-media/itphoto.c").read_text()
        # Keep the production helpers and main's full filesystem preflight;
        # replace only the ARM entrypoint, privilege setup and native API phase.
        prefix = source[:source.index('__attribute__((naked)) void _start')]
        preflight = source[source.index('int main(int argc, char **argv)'):]
        preflight = preflight[:preflight.index('    void *objc = dlopen')]
        preflight = preflight.replace('getuid()', '501')
        harness = (prefix + preflight + '    puts("ready-to-save"); return 0;\n}\n')
        harness = harness.replace('/var/mobile/Media', str(cls.media))
        (cls.work / 'receipt.c').write_text(harness)
        cls.executable = cls.work / 'receipt'
        subprocess.run(['cc', '-O1', '-Wall', '-Wextra', '-Werror',
                        '-Wno-unused-function', '-Wno-unused-variable',
                        str(cls.work / 'receipt.c'), '-o', str(cls.executable)], check=True)

    def setUp(self):
        shutil.rmtree(self.media, ignore_errors=True)
        self.staging = self.media / 'LightTouch/photo'
        self.staging.mkdir(parents=True)
        self.album = self.media / 'DCIM/100APPLE'
        self.album.mkdir(parents=True)
        self.asset = self.album / 'IMG_0001.JPG'
        self.asset.write_bytes(b'native original')
        self.image = self.staging / 'image.jpg'
        # A bounded baseline JPEG header accepted by the production preflight.
        # Native decoding is deliberately outside this harness.
        self.image.write_bytes(bytes.fromhex('ffd8 ffc0 000b 08 01e0 0280 01 01 11 00'))
        self.receipt = self.staging / '.photo-receipt'
        self.done = ('done\n' + str(self.asset) + '\n').encode()

    def run_helper(self):
        return subprocess.run([str(self.executable), 'photo'], capture_output=True)

    def assert_refused(self, content):
        self.receipt.write_bytes(content)
        result = self.run_helper()
        self.assertNotEqual(result.returncode, 0, result)
        self.assertIn(b'itphoto:', result.stderr)
        self.assertEqual(self.receipt.read_bytes(), content)
        self.assertTrue(self.image.is_file())

    def test_new_import_reaches_native_preflight_without_receipt(self):
        result = self.run_helper()
        self.assertEqual((result.returncode, result.stdout), (0, b'ready-to-save\n'))
        self.assertFalse(self.receipt.exists())
        self.assertTrue(self.image.exists())

    def test_completed_and_legacy_receipts_remove_redundant_staging(self):
        for content in (b'done\n', self.done):
            with self.subTest(content=content):
                self.image.write_bytes(b'restaged JPEG')
                self.receipt.write_bytes(content)
                result = self.run_helper()
                self.assertEqual((result.returncode, result.stdout), (0, b'already-imported\n'))
                self.assertEqual(self.receipt.read_bytes(), content)
                self.assertFalse(self.image.exists())

    def test_deleted_original_or_album_allows_reimport(self):
        self.asset.unlink()
        for missing_album in (False, True):
            with self.subTest(missing_album=missing_album):
                if missing_album:
                    self.album.rmdir()
                self.receipt.write_bytes(self.done)
                result = self.run_helper()
                self.assertEqual((result.returncode, result.stdout), (0, b'ready-to-save\n'))
                self.assertFalse(self.receipt.exists())
                self.assertTrue(self.image.exists())

    def test_uncertain_and_malformed_receipts_remain_unchanged(self):
        for content in (b'pending\n', b'', b'done\ninvalid\n', self.done[:-1],
                        self.done + b'extra\n', self.done.replace(b'IMG_0001', b'IMG_000X'),
                        self.done.replace(b'IMG_0001', b'IMG_00\x001')):
            with self.subTest(content=content):
                self.assert_refused(content)

    def test_missing_dcim_does_not_discard_completed_receipt(self):
        shutil.rmtree(self.media / 'DCIM')
        self.assert_refused(self.done)

    def test_invalid_album_does_not_discard_completed_receipt(self):
        shutil.rmtree(self.album)
        self.album.write_bytes(b'not a directory')
        self.assert_refused(self.done)

    def test_invalid_original_does_not_discard_completed_receipt(self):
        self.asset.unlink()
        self.asset.mkdir()
        self.assert_refused(self.done)


class MediaIdentity(unittest.TestCase):
    def test_location_query_recognizes_songs_and_movies(self):
        source = (ROOT / 'contrib/it-media/itmedia.c').read_text()
        statement = source.split('    const char *sql = ', 1)[1].split(';', 1)[0]
        query = ''.join(ast.literal_eval(part) for part in re.findall(r'"(?:[^"\\]|\\.)*"', statement))
        with sqlite3.connect(':memory:') as db:
            db.executescript("""
                CREATE TABLE item(pid INTEGER, is_song INTEGER);
                INSERT INTO item VALUES(1,1),(2,0);
                ATTACH DATABASE ':memory:' AS loc;
                CREATE TABLE loc.base_location(id INTEGER, path TEXT);
                CREATE TABLE loc.location(item_pid INTEGER, base_location_id INTEGER, location TEXT);
                INSERT INTO loc.base_location VALUES(1,'LightTouch/song'),(2,'LightTouch/movie');
                INSERT INTO loc.location VALUES(1,1,'audio.m4a'),(2,2,'video.mp4');
            """)
            self.assertEqual(db.execute(query, ('LightTouch/song', 'audio.m4a')).fetchall(), [(1,)])
            self.assertEqual(db.execute(query, ('LightTouch/movie', 'video.mp4')).fetchall(), [(2,)])
            self.assertEqual(db.execute(query, ('LightTouch/song', 'video.mp4')).fetchall(), [])
            self.assertEqual(db.execute(query, ('LightTouch/other', 'audio.m4a')).fetchall(), [])


if __name__ == '__main__':
    unittest.main()
