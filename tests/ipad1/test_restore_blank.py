#!/usr/bin/env python3
"""Blank restore target contains geometry/erased chips, no copied logical data."""
import importlib.util
import json
from pathlib import Path
import tempfile

root = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('restore_smoke', root / 'restore-smoke.py')
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
with tempfile.TemporaryDirectory(prefix='restore-blank-') as temp:
    source, destination = Path(temp) / 'source', Path(temp) / 'blank'
    source.mkdir()
    geometry = dict(page_bytes=4096, spare_bytes=128, pages_per_block=128,
                    blocks_per_ce=2, ce_per_bus=2, buses=2, chip_id='0xB614D5AD')
    (source / 'geometry.json').write_text(json.dumps(geometry))
    (source / 'bus0-ce0.pages').write_bytes(b'generated filesystem must not be copied')
    module.blank_nand(source, destination)
    assert json.loads((destination / 'geometry.json').read_text()) == geometry | {"storage_format": "nand-xor-ff-v2"}
    pages = sorted(destination.glob('*.pages'))
    assert len(pages) == 4
    for page in pages:
        assert page.stat().st_size == 4224 * 128 * 2
        with page.open('rb') as stream:
            assert stream.read(8192) == bytes(8192)
            stream.seek(-8192, 2)
            assert stream.read() == bytes(8192)
    try:
        module.blank_nand(source, destination)
    except FileExistsError:
        pass
    else:
        raise AssertionError('existing output accepted')
    geometry['buses'] = 3
    (source / 'geometry.json').write_text(json.dumps(geometry))
    try:
        module.blank_nand(source, Path(temp) / 'invalid')
    except ValueError:
        pass
    else:
        raise AssertionError('invalid controller geometry accepted')
print('PASS: blank v2 restore target has erased chips and measured geometry, no logical seed')
