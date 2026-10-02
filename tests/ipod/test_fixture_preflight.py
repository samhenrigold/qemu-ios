#!/usr/bin/env python3
"""Deployment declarations reject bad selected inputs before a guest starts."""
import plistlib
import contextlib
import io
from pathlib import Path
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zipfile
import fixture_preflight as F
import regress as R


class FixtureTests(unittest.TestCase):
    def test_prerequisites_reject_newer_fixture_without_launching(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            ipa = root / 'new.ipa'
            with zipfile.ZipFile(ipa, 'w') as archive:
                archive.writestr('Payload/Test.app/Info.plist',
                                plistlib.dumps({'MinimumOSVersion': '3.1'}))
            cfg = SimpleNamespace(qemu=directory, base_nand=directory,
                usbmuxd=directory, ipa=str(ipa), ipa_explicit=True,
                harness_ipa=str(ipa), harness_explicit=True,
                gles_app=None, gles_slotmap=None, gles_front_end=True,
                product_version='3.0')
            output = io.StringIO()
            with patch.object(R, 'requested_frame_references', return_value=[]), \
                 patch.object(R.Device, 'start') as start, \
                 patch.object(R, 'Procs') as procs, contextlib.redirect_stdout(output):
                self.assertEqual(R.report_prereqs(cfg), 1)
            self.assertIn('fixture requires iOS 3.1; guest is iOS 3.0', output.getvalue())
            self.assertIn('CANNOT run', output.getvalue())
            start.assert_not_called()
            procs.assert_not_called()

    def test_versions(self):
        self.assertEqual(F.version('2'), F.version('2.0.0'))
        self.assertGreater(F.version('2.1.10'), F.version('2.1.2'))
        for value in ('', '2.x', '2.-1', '2.1.0.0.0', None, 2.1):
            with self.assertRaises(ValueError):
                F.version(value)

    def test_actual_metadata_boundaries(self):
        with tempfile.TemporaryDirectory() as directory:
            app = Path(directory) / 'GLTest.app'; app.mkdir()
            ipa = Path(directory) / 'fixture.ipa'
            for value, expected in [('2.0',False), ('2.1.1',False), ('2.1.2',True), ('3.0',True), (None,True)]:
                info = {} if value is None else {'MinimumOSVersion':value}
                body = plistlib.dumps(info)
                (app / 'Info.plist').write_bytes(body)
                with zipfile.ZipFile(ipa,'w') as archive:
                    archive.writestr('Payload/Test.app/Info.plist',body)
                for path in (app,ipa):
                    self.assertEqual(bool(F.minimum_problem(path,'2.1.1')),expected)
            for body in (b'bad plist',plistlib.dumps([]),plistlib.dumps({'MinimumOSVersion':7})):
                (app/'Info.plist').write_bytes(body)
                with self.assertRaises((ValueError,plistlib.InvalidFileException)):
                    F.minimum_problem(app,'2.1.1')

    def test_missing_defaults_and_explicit_inputs(self):
        cfg = SimpleNamespace(ipa='/absent',ipa_explicit=False,harness_ipa=None,gles_app=None,
                              gles_slotmap=None,gles_front_end=False,product_version='2.1.1')
        self.assertEqual(F.requested_problems(cfg,['appinstall','audio','gles'],'/absent','/absent'),[])
        cfg.ipa_explicit=True
        self.assertIn('missing explicit fixture',F.requested_problems(cfg,['appinstall'],'/absent','/absent')[0])

    def test_explicit_unknown_guest_and_multi_app_ipa(self):
        with tempfile.TemporaryDirectory() as directory:
            ipa=Path(directory)/'test.ipa'
            cfg=SimpleNamespace(ipa=str(ipa),ipa_explicit=True,harness_ipa=None,gles_app=None,
                                gles_slotmap=None,gles_front_end=False,product_version=None)
            with zipfile.ZipFile(ipa,'w') as archive:
                archive.writestr('Payload/Test.app/Info.plist',plistlib.dumps({'MinimumOSVersion':'2.0'}))
            self.assertIn('cannot validate',F.requested_problems(cfg,['appinstall'],'absent','absent')[0])
            cfg.product_version='2.1.1'
            with zipfile.ZipFile(ipa,'a') as archive:
                archive.writestr('Payload/Other.app/Info.plist',plistlib.dumps({'MinimumOSVersion':'2.0'}))
            self.assertIn('exactly one',F.requested_problems(cfg,['appinstall'],'absent','absent')[0])

    def test_main_malformed_fixture_never_launches_or_mutates(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);ipa=root/'bad.ipa';out=root/'must-not-exist'
            with zipfile.ZipFile(ipa,'w') as archive:
                archive.writestr('Payload/Test.app/Info.plist',b'<?xml version="1.0"?><plist><dict><key>MinimumOSVersion</key>')
            def configure(cfg):
                cfg.product_version='2.1.1';cfg.gles_front_end=False;cfg.board='n72ap'
            with patch('sys.argv',['regress.py','--harness-ipa',str(ipa),'--checks','audio','--out',str(out)]),\
                 patch.object(R,'configure_device',side_effect=configure),patch.object(R.Device,'start') as start,\
                 patch.object(R,'Procs') as procs:
                with self.assertRaisesRegex(SystemExit,'cannot validate'):
                    R.main()
                start.assert_not_called();procs.assert_not_called();self.assertFalse(out.exists())

    def test_device_configuration_preserves_patch_version(self):
        import json
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);(root/'device.lock.json').write_text(json.dumps({'product_version':'2.1.10'}))
            cfg=SimpleNamespace(files_dir=directory,device=directory,base_nand=None,nor=None,
                                direct_iboot=None,gid_blobs=None)
            R.configure_device(cfg)
            self.assertEqual(cfg.product_version,'2.1.10')
            self.assertEqual(cfg.device_version,(2,1))

    def test_main_incompatible_explicit_fixture_never_launches_or_mutates(self):
        with tempfile.TemporaryDirectory() as directory:
            root=Path(directory);ipa=root/'new.ipa';out=root/'must-not-exist'
            with zipfile.ZipFile(ipa,'w') as archive:
                archive.writestr('Payload/Test.app/Info.plist',plistlib.dumps({'MinimumOSVersion':'2.1.2'}))
            def configure(cfg):
                cfg.product_version='2.1.1';cfg.gles_front_end=False;cfg.board='n72ap'
            args=['regress.py','--ipa',str(ipa),'--checks','appinstall','--out',str(out)]
            with patch('sys.argv',args),patch.object(R,'configure_device',side_effect=configure),\
                 patch.object(R.Device,'start') as start,patch.object(R,'Procs') as procs:
                with self.assertRaisesRegex(SystemExit,'requires iOS 2.1.2'):
                    R.main()
                start.assert_not_called();procs.assert_not_called();self.assertFalse(out.exists())


if __name__=='__main__':
    unittest.main()
