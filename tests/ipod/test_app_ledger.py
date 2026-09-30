#!/usr/bin/env python3
"""Ledger verdicts must not promote skipped checks or inferred visual quality."""
import json
from pathlib import Path
import plistlib
import tempfile
from types import SimpleNamespace
from unittest.mock import Mock, patch
import zipfile
import app_ledger as L
import regress as R

passed = {key: {'ok': True, 'xfail': False, 'detail': 'passed'} for key in ('boot','appinstall','applaunch')}
assert L.verdict(passed,0)==('yes (30 s)','')
for value in (None,False):
    assert L.verdict(dict(passed,applaunch={'ok':value,'detail':'unverified'}),0)[0]=='not verified'
assert L.verdict({},0)[0]=='not verified'
assert L.verdict(passed,1)[0]=='not verified'
with tempfile.TemporaryDirectory() as temporary:
    root=Path(temporary);inputs=root/'inputs';inputs.mkdir()
    ipa=inputs/'App | name.IPA'
    with zipfile.ZipFile(ipa,'w') as archive:
        archive.writestr('Payload/App.app/Info.plist',plistlib.dumps({'CFBundleIdentifier':'org.example.App'}))
    (inputs/'broken.ipa').write_bytes(b'not a zip')
    cfg=SimpleNamespace(ledger=str(inputs),out=str(root/'output'))
    commands=[]
    def child(command,**kwargs):
        commands.append(command)
        output=Path(command[command.index('--out')+1]);output.mkdir()
        (output/'results.json').write_text(json.dumps(passed))
        return SimpleNamespace(returncode=0)
    with patch.object(L.subprocess,'run',side_effect=child):
        assert L.run_ledger(cfg)==1
    rows=json.loads((root/'output/ledger.json').read_text())['apps']
    assert rows[0]['runs']=='yes (30 s)' and rows[0]['sha256']==L.digest(ipa)
    assert all(rows[0][key]=='unreviewed' for key in ('renders','audio','input','network'))
    assert rows[1]['runs']=='not verified' and rows[1]['blocker']
    assert len(commands)==1 and '--launch-stages' in commands[0] and '--clean' not in commands[0]
    assert 'App \\| name' in (root/'output/ledger.md').read_text()
    try: L.run_ledger(cfg);raise AssertionError('overwrote prior review')
    except FileExistsError: pass
    R.START=R.time.time()
    R.finish({key:SimpleNamespace(ok=True,skipped=False,xfail=False,detail='passed',name=key) for key in passed},Mock(),SimpleNamespace(out=str(root)))
    assert json.loads((root/'results.json').read_text())=={key:dict(value,skipped=False) for key,value in passed.items()}
    dev=SimpleNamespace(dir=str(root),qmp=Mock())
    response=SimpleNamespace(returncode=0,stdout='',stderr='')
    with patch.object(R,'prepare_app_control',return_value=22),patch.object(R,'ipa_bundle_id',return_value='org.example.App'), \
         patch.object(R,'unlock',return_value=(True,'')),patch.object(R,'springboard',return_value=response), \
         patch.object(R,'foreground_is',return_value=True),patch.object(R,'to_png'),patch.object(R,'lit_count',return_value=(255,300000)), \
         patch.object(R.shutil,'which',return_value='/usr/local/bin/idevicesyslog'),patch.object(R,'mux_env',return_value={}), \
         patch.object(R,'run') as run,patch.object(R,'guest_file',return_value=b'plist'), \
         patch.object(R.time,'monotonic',side_effect=[100,100,105,120]),patch.object(R.time,'sleep') as sleep:
        procs=Mock()
        assert R.check_applaunch(SimpleNamespace(ipa=str(ipa),launch_stages=True),procs,dev,R.Result('applaunch'))
        # syslog capture started 3 s before the launch; stages at 5 s and 20 s, verdict at 30 s
        assert procs.spawn.call_args.args[0]==['idevicesyslog']
        assert [call.args[0] for call in sleep.call_args_list]==[3,5,15,10]
        assert run.call_args.args[0][:2]==['idevicecrashreport','-k']
        assert (root/'installation.plist').read_bytes()==b'plist'
        assert [Path(call.args[0]).name for call in dev.qmp.shot.call_args_list]==['app-5s.ppm','app-20s.ppm','app.ppm']
print('PASS: exact-file identity, failed/skipped checks, review boundaries, durable progress, launch timing and preserved evidence')
