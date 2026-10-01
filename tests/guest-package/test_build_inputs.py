#!/usr/bin/env python3
"""Guest cache dependencies: new sources and same-size SDK changes invalidate reuse."""
import importlib.util
import os
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('build_inputs', Path(__file__).resolve().parents[2] / 'contrib/guest-package/build_inputs.py')
inputs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inputs)


class BuildInputsTests(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        for name in ('include/hw/arm/guest-services/gles-names.h', 'contrib/guest-package/VERSION',
                     'contrib/export-guest-artifacts.sh', 'contrib/macos-app/entitlements.plist'):
            self.put(name)

    def put(self, name, content=b'input'):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        return path

    def test_new_nested_source_is_a_dependency_but_generated_macho_is_not(self):
        before = inputs.sources(self.root)
        self.put('contrib/it-agent/data/new-input.dat')
        self.put('contrib/it-agent/it_agent', b'\xce\xfa\xed\xfecompiled')
        self.put('contrib/it-agent/agent.o')
        self.put('contrib/it-gles/gles_stubs.h')
        self.put('contrib/it-agent/__pycache__/cached.pyc')
        after = inputs.sources(self.root)
        self.assertEqual(set(after) - set(before), {'contrib/it-agent/data/new-input.dat'})
        (self.root / 'contrib/it-agent/data/new-input.dat').unlink()
        self.assertEqual(inputs.sources(self.root), before)

    def test_exported_headers_entitlements_and_validation_recipes_are_inputs(self):
        for name in ('contrib/ios-app/api.h', 'contrib/macos-app/api.h', 'tests/guest-package/test_format.py'):
            self.put(name)
            self.assertIn(name, inputs.sources(self.root))
        before = inputs.sources(self.root)
        self.put('contrib/macos-app/entitlements.plist', b'changed')
        self.assertNotEqual(inputs.sources(self.root), before)

    def test_sdk_change_is_detected_even_when_size_and_mtime_are_preserved(self):
        path = self.put('SDK/usr/include/header.h', b'old')
        before = inputs.tree(self.root / 'SDK')
        stat = path.stat()
        path.write_bytes(b'new')
        os.utime(path, ns=(stat.st_atime_ns, stat.st_mtime_ns))
        self.assertNotEqual(inputs.tree(self.root / 'SDK'), before)

    def test_sdk_inventory_and_executable_modes_are_dependencies(self):
        path = self.put('SDK/tool')
        before = inputs.tree(self.root / 'SDK')
        path.chmod(0o755)
        self.assertNotEqual(inputs.tree(self.root / 'SDK'), before)
        self.put('SDK/additional-header.h')
        self.assertEqual(inputs.tree(self.root / 'SDK')['files'], 2)
        path.unlink()
        self.assertEqual(inputs.tree(self.root / 'SDK')['files'], 1)
        with self.assertRaisesRegex(ValueError, 'empty build input'):
            inputs.tree(self.root / 'absent')


if __name__ == '__main__':
    unittest.main()
