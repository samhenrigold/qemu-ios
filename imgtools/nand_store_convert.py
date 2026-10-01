#!/usr/bin/env python3
"""Offline K48 page-store conversion to explicit sparse XOR-FF NAND v2.

Research CLI: caller must hold exclusive stopped-device ownership. This does not
convert a writable overlay or repair a guest FTL. Original files are unchanged.
Legacy all-zero strides are intrinsically ambiguous; choose their interpretation
explicitly, or let the default reject them. Production ownership belongs in the
shared device/storage API, not this research utility.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import tempfile

V1 = 'legacy-zero-blank-v1'
V2 = 'nand-xor-ff-v2'
XOR = bytes.maketrans(bytes(range(256)), bytes(255 - n for n in range(256)))


def convert(source: Path, destination: Path, zero_pages='reject'):
    if destination.exists():
        raise ValueError('destination already exists')
    if zero_pages not in ('reject', 'erased', 'data'):
        raise ValueError('invalid zero-page policy')
    geometry_bytes = (source / 'geometry.json').read_bytes()
    geometry = json.loads(geometry_bytes)
    if geometry.get('storage_format', V1) != V1:
        raise ValueError('source must be a legacy v1 store')
    if any(source.glob('*.dirty')):
        raise ValueError('convert the authoritative base, not an overlay')
    fields = ('page_bytes', 'spare_bytes', 'pages_per_block', 'blocks_per_ce',
              'buses', 'ce_per_bus')
    if any(type(geometry.get(k)) is not int or geometry[k] <= 0 for k in fields):
        raise ValueError('invalid NAND geometry')
    stride = geometry['page_bytes'] + geometry['spare_bytes']
    pages = geometry['pages_per_block'] * geometry['blocks_per_ce']
    if stride > 0x4000 or pages > 0xffffffff or geometry['buses'] != 2 or geometry['ce_per_bus'] > 8:
        raise ValueError('geometry exceeds controller limits')
    files = [f'bus{bus}-ce{ce}.pages' for bus in range(geometry['buses'])
             for ce in range(geometry['ce_per_bus'])]
    # Never follow source symlinks and never reinterpret a truncated file.
    for name in files:
        path = source / name
        if path.is_symlink() or path.stat().st_size != stride * pages:
            raise ValueError(f'{name}: source size/type differs from geometry')
    temp = Path(tempfile.mkdtemp(prefix=destination.name + '.convert-', dir=destination.parent))
    counts = {'zero_pages': 0, 'nonzero_pages': 0}
    hashes = {}
    try:
        for name in files:
            digest = hashlib.sha256()
            with (source / name).open('rb') as src, (temp / name).open('xb') as dst:
                dst.truncate(stride * pages)
                for page in range(pages):
                    data = src.read(stride)
                    if len(data) != stride:
                        raise ValueError(f'{name}: source changed during conversion')
                    digest.update(data)
                    if not any(data):
                        counts['zero_pages'] += 1
                        if zero_pages == 'reject':
                            raise ValueError(f'{name} page {page}: ambiguous legacy zero stride; select erased or data explicitly')
                        encoded = b'' if zero_pages == 'erased' else b'\xff' * stride
                    else:
                        counts['nonzero_pages'] += 1
                        encoded = data.translate(XOR)
                    if any(encoded):
                        dst.seek(page * stride)
                        dst.write(encoded)
                dst.flush()
                os.fsync(dst.fileno())
            hashes[name] = digest.hexdigest()
        if (source / 'geometry.json').read_bytes() != geometry_bytes:
            raise ValueError('source geometry changed during conversion')
        # A second read detects writes during the conversion; callers still
        # require exclusive ownership so a writer cannot race publication.
        for name in files:
            verify = hashlib.sha256()
            with (source / name).open('rb') as stream:
                while data := stream.read(1024 * 1024):
                    verify.update(data)
            if verify.hexdigest() != hashes[name]:
                raise ValueError(f'{name}: source changed during conversion')
        geometry['storage_format'] = V2
        (temp / 'geometry.json').write_text(json.dumps(geometry, indent=2) + '\n')
        report = {'source_geometry_sha256': hashlib.sha256(geometry_bytes).hexdigest(),
                  'source_sha256': hashes, 'storage_format': V2,
                  'legacy_zero_pages': zero_pages, **counts}
        (temp / 'conversion.json').write_text(json.dumps(report, indent=2) + '\n')
        # rename does not replace a nonempty existing directory, but check
        # destination explicitly to also reject a concurrent empty directory.
        if destination.exists():
            raise ValueError('destination appeared during conversion')
        os.rename(temp, destination)
        return report
    except BaseException:
        shutil.rmtree(temp, ignore_errors=True)
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path)
    parser.add_argument('destination', type=Path)
    parser.add_argument('--legacy-zero-pages', choices=('reject', 'erased', 'data'), default='reject')
    args = parser.parse_args()
    try:
        print(json.dumps(convert(args.source, args.destination, args.legacy_zero_pages), indent=2))
    except (ValueError, OSError) as error:
        parser.exit(1, f'{error}\n')


if __name__ == '__main__':
    main()
