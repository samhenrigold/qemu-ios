#!/usr/bin/env python3
"""The native iPod visual gate uses exact firmware references, never family PNGs."""
import ast
import hashlib
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace

root = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('frame_reference', root / 'tests/frame_reference.py')
references = importlib.util.module_from_spec(spec)
spec.loader.exec_module(references)

with tempfile.TemporaryDirectory(prefix='ipod-frame-reference-') as temp:
    directory = Path(temp)
    (directory / '1x-home.png').write_bytes(b'borrowed 1.1 picture')
    def select(build='4B1', version='1.1.5', scene='home', board='n45ap'):
        return references.qualified(directory, board, build, version, scene)
    assert 'missing qualified' in select()[1]
    ref = directory / 'n45ap/4B1'; ref.mkdir(parents=True)
    picture = ref / 'home.png'; picture.write_bytes(b'unit-only reference payload')
    manifest = dict(board='n45ap', build='4B1', product_version='1.1.5',
                    baseline=dict(renderer='stock-software-layerkit', provenance='synthetic unit fixture only'),
                    frames=dict(home=dict(sha256=hashlib.sha256(picture.read_bytes()).hexdigest(),
                                          source_sha256='0' * 64)))
    path = ref / 'reference.json'
    path.write_text(json.dumps(manifest))
    assert select() == (str(picture), None)
    manifest['product_version'] = None; path.write_text(json.dumps(manifest))
    assert 'missing full firmware' in select(version=None)[1]
    assert 'missing full firmware' in select(version='')[1]
    manifest['product_version'] = '1.1.5'; path.write_text(json.dumps(manifest))
    assert 'missing qualified' in select('3A101a', '1.1')[1]
    assert 'identity differs' in select(version='1.1')[1]
    assert 'missing qualified' in select(scene='opened-app')[1]
    assert 'missing firmware' in select(build='../4B1')[1]
    assert 'invalid scene' in select(scene='../home')[1]
    manifest['board'] = 'n72ap'; path.write_text(json.dumps(manifest))
    assert 'identity differs' in select()[1]
    manifest['board'] = 'n45ap'; manifest['baseline']['renderer'] = 'candidate-gl'
    path.write_text(json.dumps(manifest)); assert 'independent' in select()[1]
    manifest['baseline']['renderer'] = 'stock-software-layerkit'
    path.write_text(json.dumps(manifest)); picture.write_bytes(b'tampered')
    assert 'hash differs' in select()[1]

# Compile the actual native front-end gate, with hardware/services doubled only.
# All liveness conditions pass; absent build-scoped visual coverage must fail.
source = ast.parse((root / 'tests/ipod/regress.py').read_text())
function = next(n for n in source.body if isinstance(n, ast.FunctionDef) and n.name == 'check_gles_front_end')
preflight = next(n for n in source.body if isinstance(n, ast.FunctionDef) and n.name == 'requested_frame_references')
class Result:
    def set(self, ok, detail): self.ok, self.detail = ok, detail
class QMP:
    def tap(self, *a): pass
    def swipe(self, *a, **kw): pass
    def home(self): pass
    def shot(self, path): return path
    def cmd(self, *a, **kw): return 1
with tempfile.TemporaryDirectory(prefix='ipod-native-reference-') as temp:
    directory = Path(temp)
    (directory / 'qemu.log').write_text('[gles] OpenGLES front end (unit)\nCoreAnimation composites through the host\n')
    dev = SimpleNamespace(dir=temp, qmp=QMP(), alive=lambda: True)
    cfg = SimpleNamespace(board='n45ap', build='4B1', product_version='1.1.5', home_lit_min=100)
    scope = dict(frame_reference=references, GLES_REFS=temp,
                 os=__import__('os'), time=SimpleNamespace(sleep=lambda _: None),
                 to_png=lambda *_: None, lit_count=lambda p: (0, 2000 if 'opened-app' in p else 500),
                 itqmp=SimpleNamespace(gles_rejects=lambda _: {}),
                 framecheck=SimpleNamespace(verdict=lambda *_: (_ for _ in ()).throw(AssertionError('unqualified image compared'))))
    exec(compile(ast.Module(body=[function,preflight], type_ignores=[]), 'actual-iPod-visual-gate', 'exec'), scope)
    cfg.gles_front_end=True
    assert len(scope['requested_frame_references'](cfg,['boot','gles']))==3
    assert scope['requested_frame_references'](cfg,['boot'])==[]
    cfg.gles_front_end=False
    assert scope['requested_frame_references'](cfg,['gles'])==[]
    result=Result();scope['check_gles_front_end'](cfg,dev,result)
    assert not result.ok and 'visual coverage missing' in result.detail,result.detail
print('PASS: exact board/build/full-version references, provenance/hash refusal, no borrowed family fallback, actual native gate refuses unqualified liveness')


# The newly reviewed exact-build pictures retain the same sensitivity as the
# historical audit oracle: orientation, channel order and stale scenes fail.
from PIL import Image, ImageOps
spec = importlib.util.spec_from_file_location('actual_framecheck', root / 'tests/framecheck.py')
checker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checker)
with tempfile.TemporaryDirectory(prefix='exact-frame-mutants-') as temp:
    for board, build, version, other_build, other_version in [
        ('n45ap', '3A101a', '1.1', '4B1', '1.1.5'),
        ('n45ap', '4B1', '1.1.5', '3A101a', '1.1'),
        ('n72ap', '5F138', '2.1.1', '7A341', '3.0'),
        ('n72ap', '7A341', '3.0', '5F138', '2.1.1')]:
        home, why = references.qualified(root / 'tests/gles-refs', board, build, version, 'home')
        safari, safari_why = references.qualified(root / 'tests/gles-refs', board, build, version, 'opened-app')
        assert home and safari and not why and not safari_why
        picture = Image.open(home).convert('RGB')
        flipped = Path(temp) / (build + '-flip.png')
        swapped = Path(temp) / (build + '-channels.png')
        ImageOps.flip(picture).save(flipped)
        red, green, blue = picture.split()
        Image.merge('RGB', (blue, green, red)).save(swapped)
        assert not checker.verdict(flipped, home)['ok']
        assert not checker.verdict(swapped, home)['ok']
        assert not checker.verdict(safari, home)['ok']
        other, why = references.qualified(root / 'tests/gles-refs', board, other_build, other_version, 'home')
        assert other and not why and not checker.verdict(other, home)['ok']
print('PASS: all four exact1.x/2.1.1/3.0 oracles reject flip, channel swap, stale scene and borrowed build')
