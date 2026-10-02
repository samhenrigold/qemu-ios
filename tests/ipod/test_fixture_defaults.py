#!/usr/bin/env python3
"""Version-aware fixture defaults and ledger forwarding use actual host code."""
import hashlib
import json
from pathlib import Path
import plistlib
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile

ROOT = Path(__file__).resolve().parents[2]
sys.path.extend([str(ROOT / 'imgtools'), str(ROOT / 'tests'), str(ROOT / 'tests/ipod')])
import app_ledger as L
import fixture_preflight as F
import regress as R


def write_ipa(path):
    info = {'MinimumOSVersion': '2.0', 'CFBundleIdentifier': 'test'}
    with zipfile.ZipFile(path, 'w') as archive:
        archive.writestr('Payload/Test.app/Info.plist', plistlib.dumps(info))


class FixtureDefaultsTests(unittest.TestCase):
    def test_version_family_and_independent_explicit_overrides(self):
        cases = [('2.1.1', 'ios2'), ('2.2.1', 'ios2'), ('3.0', 'full'),
                 ('3.1.3', 'full'), (None, 'full')]
        overrides = {'ipa': '/explicit/ipa', 'harness': '/explicit/harness',
                     'gles': '/explicit/app', 'slotmap': '/explicit/map'}
        for guest, flavor in cases:
            with self.subTest(guest=guest):
                chosen = F.select_inputs(ROOT, guest)
                self.assertEqual(chosen['flavor'], flavor)
                self.assertFalse(any(chosen['explicit'].values()))
                self.assertEqual('build-ios2' in chosen['ipa'], flavor == 'ios2')
                chosen = F.select_inputs(ROOT, guest, **overrides)
                for role, path in overrides.items():
                    self.assertEqual(chosen[role], path)
                self.assertTrue(all(chosen['explicit'].values()))

    def test_missing_selected_legacy_stops_before_launch_or_output(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            output = root / 'must-not-exist'

            def configure(cfg):
                cfg.product_version = '2.1.1'
                cfg.gles_front_end = True
                cfg.board = 'n72ap'

            args = ['regress.py', '--checks', 'audio', '--out', str(output)]
            with patch('sys.argv', args), patch.object(R, 'ROOT', str(root)), \
                    patch.object(R, 'configure_device', side_effect=configure), \
                    patch.object(R, 'Procs') as procs:
                with self.assertRaisesRegex(SystemExit, 'missing ios2 fixture'):
                    R.main()
                procs.assert_not_called()
                self.assertFalse(output.exists())

    def test_springboard_leg_has_no_unused_app_inputs(self):
        cfg = SimpleNamespace(
            product_version='2.1.1', gles_front_end=True, ipa='/absent',
            ipa_explicit=False, harness_ipa='/absent', harness_explicit=False,
            gles_app='/absent', gles_explicit=False, gles_slotmap='/absent',
            slotmap_explicit=False, fixture_flavor='ios2')
        self.assertEqual(F.requested_problems(cfg, ['gles'], '/absent', '/absent'), [])
        self.assertEqual(F.selected_metadata(cfg, ['gles'])['inputs'], [])

    def test_receipts_identify_exact_scoped_files(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ipa = root / 'fixture.ipa'
            write_ipa(ipa)
            app = root / 'GLTest.app'
            app.mkdir()
            (app / 'GLTest').write_bytes(b'actual binary bytes')
            info = {'MinimumOSVersion': '2.0', 'CFBundleExecutable': 'GLTest'}
            (app / 'Info.plist').write_bytes(plistlib.dumps(info))
            slotmap = root / 'slotmap'
            slotmap.write_bytes(b'1 glClear\n')
            cfg = SimpleNamespace(
                product_version='2.1.1', gles_front_end=False, ipa=str(ipa),
                ipa_explicit=False, harness_ipa=str(ipa), harness_explicit=False,
                gles_app=str(app), gles_explicit=False, gles_slotmap=str(slotmap),
                slotmap_explicit=False, fixture_flavor='ios2')
            receipt = F.selected_metadata(cfg, ['appinstall', 'audio', 'gles'])
            roles = {row['role']: row for row in receipt['inputs']}
            self.assertEqual(roles['ipa']['sha256'], hashlib.sha256(ipa.read_bytes()).hexdigest())
            self.assertEqual(roles['gles']['executableSHA256'],
                             hashlib.sha256((app / 'GLTest').read_bytes()).hexdigest())
            self.assertEqual(roles['gles']['plistSHA256'],
                             hashlib.sha256((app / 'Info.plist').read_bytes()).hexdigest())
            self.assertEqual(roles['slotmap']['sha256'], hashlib.sha256(slotmap.read_bytes()).hexdigest())
            self.assertNotIn('sha256', roles['gles'])  # No fabricated whole-bundle hash.
            self.assertEqual(roles['ipa']['minimumOS'], '2.0')
            self.assertIn('no native runtime success', receipt['limits'])

    def test_real_lock_resolution_preserves_default_springboard_leg(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            device = root / 'device'
            device.mkdir()
            lock = {'product_version': '2.1.1', 'build': '5F138',
                    'derived': {'gles_engine': 'OpenGLES', 'guest_tools': 'none'}}
            (device / 'device.lock.json').write_text(json.dumps(lock))
            output = root / 'frontend-out'
            args = ['regress.py', '--device', str(device), '--checks', 'gles',
                    '--qemu', str(root / 'absent-qemu'), '--out', str(output)]
            with patch('sys.argv', args), patch.object(R, 'ROOT', str(root)), \
                    patch.object(R, 'Procs') as procs, \
                    patch.object(R, 'requested_frame_references', wraps=R.requested_frame_references) as references:
                with self.assertRaisesRegex(SystemExit, 'missing qualified frame reference'):
                    R.main()
                procs.assert_not_called()
            self.assertFalse(output.exists())
            actual_cfg = references.call_args.args[0]
            self.assertTrue(actual_cfg.gles_front_end)
            receipt = F.selected_metadata(actual_cfg, ['gles'])
            self.assertEqual(receipt['inputs'], [])
            self.assertIn('no native runtime success', receipt['limits'])

    def test_actual_parser_and_ledger_generator_exclude_internal_flags(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            inputs = root / 'inputs'
            inputs.mkdir()
            ipa = inputs / 'test.ipa'
            write_ipa(ipa)
            commands = []

            def child(command, **kwargs):
                commands.append(command)
                output = Path(command[command.index('--out') + 1])
                output.mkdir()
                results = {check: {'ok': True, 'detail': 'passed', 'xfail': False}
                           for check in ('boot', 'appinstall', 'applaunch')}
                (output / 'results.json').write_text(json.dumps(results))
                return SimpleNamespace(returncode=0)

            args = ['regress.py', '--ledger', str(inputs), '--ipa', str(ipa),
                    '--out', str(root / 'ledger-out')]
            # Parser and argument generator are real; only the owned child is mocked.
            with patch('sys.argv', args), patch.object(L.subprocess, 'run', side_effect=child):
                self.assertEqual(R.main(), 0)
            self.assertEqual(len(commands), 1)
            self.assertNotIn('--ipa-explicit', commands[0])
            self.assertNotIn('--fixture-flavor', commands[0])


if __name__ == '__main__':
    unittest.main()
