#!/usr/bin/env python3
"""Validate the explicit gate inventory; emit quick-tier scheduling records.

Registration describes prerequisites, not availability guessed from test source.
Manual checks remain visible as SKIP; a new test must be registered before any
 gate tier runs. A test exiting with a usage error is still a failed test.
"""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TIERS = {'quick', 'models', 'full', 'fresh', 'manual'}


def validate(root, registry):
    if registry.get('version') != 1:
        raise ValueError('unsupported gate registry version')
    entries = registry['tests']
    names = set()
    for entry in entries:
        name = entry['name']
        if name in names:
            raise ValueError(f'duplicate registration: {name}')
        names.add(name)
        if entry['tier'] not in TIERS:
            raise ValueError(f'invalid tier: {name}')
        prerequisites = entry['prerequisites']
        if not isinstance(prerequisites, list) or any(not isinstance(p, str) or not p for p in prerequisites):
            raise ValueError(f'invalid prerequisites: {name}')
        if any(c in name for c in '\t\n') or any(c in entry.get('reason', '') for c in '\t\n'):
            raise ValueError(f'invalid scheduling record: {name}')
        if entry['tier'] == 'manual' and (not prerequisites or not entry.get('reason')):
            raise ValueError(f'manual check must declare inputs and reason: {name}')
        if entry['tier'] != 'manual' and prerequisites and entry.get('missing') not in {'fail', 'skip'}:
            raise ValueError(f'input-dependent check must declare missing policy: {name}')
        if not name.startswith('qtest/') and not (root / name.split(' ')[0]).is_file():
            raise ValueError(f'registered test missing: {name}')
    discovered = {str(path.relative_to(root)) for directory in ('ipod', 'ipad1', 'guest-package')
                  for path in (root / 'tests' / directory).glob('test_*.py')}
    missing = discovered - names
    if missing:
        raise ValueError('unregistered tests: ' + ', '.join(sorted(missing)))
    return entries


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--quick-plan', action='store_true')
    parser.add_argument('--tier-plan', choices=sorted(TIERS - {'manual'}))
    args = parser.parse_args()
    try:
        entries = validate(ROOT, json.loads((ROOT / 'tests/gate-registry.json').read_text()))
    except (ValueError, KeyError, TypeError, OSError) as error:
        parser.exit(1, f'gate registration failed: {error}\n')
    if args.quick_plan or args.tier_plan:
        for entry in entries:
            if entry['tier'] in ({'quick', 'manual'} if args.quick_plan else {args.tier_plan}):
                print('\t'.join((entry['tier'], entry['name'], entry.get('reason', ''))))


if __name__ == '__main__':
    main()
