"""Sequential, isolated IPA launch evidence; visual/audio reviews stay explicit."""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from datetime import datetime, timezone


def digest(path):
    checksum = hashlib.sha256()
    with open(path, 'rb') as file:
        for block in iter(lambda: file.read(65536), b''):
            checksum.update(block)
    return checksum.hexdigest()


def verdict(results, returncode):
    required = ('boot', 'appinstall', 'applaunch')
    failures = [name + ': ' + results.get(name, {}).get('detail', 'no completed check')
                for name in required if results.get(name, {}).get('ok') is not True]
    if returncode and not failures:
        failures.append('harness did not finish successfully')
    return ('not verified', '; '.join(failures)) if failures else ('yes (30 s)', '')


def write_ledger(output, rows):
    payload = {'generated_at': datetime.now(timezone.utc).isoformat(), 'apps': rows}
    temporary = output / 'ledger.json.next'
    temporary.write_text(json.dumps(payload, indent=2) + '\n')
    temporary.replace(output / 'ledger.json')
    def cell(value):
        return ' '.join(str(value).split()).replace('|', r'\|')
    lines = ['# App launch ledger', '',
             'Launch checks are automatic. Rendering, audio, input and network behavior require review.', '',
             '| App | Runs | Renders | Audio | Input | Network | Blocker | Evidence |',
             '| --- | --- | --- | --- | --- | --- | --- | --- |']
    for row in rows:
        evidence = '[run](' + row['run'] + '/)' if row.get('run') else '—'
        lines.append('| ' + ' | '.join(cell(row[key]) for key in
            ('app', 'runs', 'renders', 'audio', 'input', 'network', 'blocker')) + ' | ' + evidence + ' |')
    temporary = output / 'ledger.md.next'
    temporary.write_text('\n'.join(lines) + '\n')
    temporary.replace(output / 'ledger.md')


def run_ledger(cfg):
    import regress as r
    directory = Path(cfg.ledger).expanduser().resolve()
    files = sorted(p for p in directory.iterdir() if p.is_file() and p.suffix.lower() == '.ipa')
    if not files:
        raise ValueError('The ledger directory contains no IPA files')
    output = Path(cfg.out).expanduser().resolve() if cfg.out else Path(tempfile.mkdtemp(prefix='it-ledger-'))
    if cfg.out:
        output.mkdir(parents=True, exist_ok=False)  # Never replace an earlier review.
    rows = []
    for index, ipa in enumerate(files):
        row = dict(app=ipa.stem, ipa=str(ipa), runs='not verified', renders='unreviewed',
                   audio='unreviewed', input='unreviewed', network='unreviewed', blocker='')
        print(f'[{index+1}/{len(files)}] {ipa.name}', flush=True)
        try:
            row['sha256'] = digest(ipa)
            row['bundle_id'] = r.ipa_bundle_id(str(ipa))
            run = f'{index+1:04d}-{row["sha256"][:12]}'
            row['run'] = run
            command = [sys.executable, str(Path(r.__file__).resolve()), '--ipa', str(ipa),
                       '--out', str(output/run), '--checks', 'boot,appinstall,applaunch', '--launch-stages']
            owned = {'ledger', 'out', 'ipa', 'checks', 'launch_stages', 'clean',
                     'quick', 'with_apps', 'check_prereqs'}
            for field, value in vars(cfg).items():
                if field in owned or value is None or value is False:
                    continue
                command.append('--' + field.replace('_', '-'))
                if value is not True:
                    command.append(str(value))
            with (output/(run+'.log')).open('w') as log:
                completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            result_path = output/run/'results.json'
            results = json.loads(result_path.read_text()) if result_path.exists() else {}
            row['runs'], row['blocker'] = verdict(results, completed.returncode)
            if digest(ipa) != row['sha256']:
                row['runs'], row['blocker'] = 'not verified', 'IPA changed during the run'
        except (OSError, ValueError, r.zipfile.BadZipFile, r.ExpatError, RuntimeError) as error:
            row['blocker'] = str(error)
        rows.append(row)
        write_ledger(output, rows)
        print(row['runs'] + (': ' + row['blocker'] if row['blocker'] else ''), flush=True)
    print('Ledger: ' + str(output/'ledger.md'), flush=True)
    return int(any(row['runs'] != 'yes (30 s)' for row in rows))
