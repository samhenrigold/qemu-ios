#!/usr/bin/env python3
"""The compatibility entry point locates inputs and delegates all work to Swift."""
import importlib.util
import json
from pathlib import Path
import tempfile
from types import SimpleNamespace
root=Path(__file__).resolve().parents[2]
spec=importlib.util.spec_from_file_location('device_cli',root/'imgtools/device.py')
device=importlib.util.module_from_spec(spec);spec.loader.exec_module(device)
with tempfile.TemporaryDirectory(prefix='device-cli-') as tmp:
    d=Path(tmp);catalog=d/'catalog.json';manifest=d/'manifest.json'
    entry={'id':'k48ap-7B500','product_type':'iPad1,1','source':{'sha1':'abc'},
           'recipe':{'storage':'16g','system_mib':1280,'data_size':'partition','options':{'appsync':True}}}
    catalog.write_text(json.dumps({'format':1,'entries':[entry]}))
    m={'format':1,'board':'k48ap','build':'7B500','product_type':'iPad1,1',
       'ipsw':{'path':'~/test.ipsw','sha1':'abc'},'identity':{'seed':'seed'},'options':{'appsync':True}}
    manifest.write_text(json.dumps(m))
    a=SimpleNamespace(catalog=str(catalog),firmwarekit='/test/firmwarekit',manifest=str(manifest),outdir=str(d/'out'),
       id=None,ipsw=None,out=None,seed=None,helper='/test/helper',guest_tools='/test/tools',cache=None,
       sibling_entry=None,sibling_ipsw=None,gl_test=True)
    cmd=device.command(a)
    assert cmd[:2]==['/test/firmwarekit','create']
    assert cmd[cmd.index('--id')+1]=='k48ap-7B500'
    assert cmd[cmd.index('--seed')+1]=='seed'
    assert cmd[cmd.index('--helper')+1]=='/test/helper' and '--gl-test' in cmd
    assert '--activation-hook' not in cmd and '--keys' not in cmd
    for change in ({'options':{'appsync':False}},{'activation':{'hook':'/custom'}},{'ipsw':{'path':'x','sha1':'wrong'}}):
        manifest.write_text(json.dumps(m|change))
        try: device.command(a)
        except ValueError: pass
        else: raise AssertionError('conflicting legacy policy accepted')
    a.manifest=a.outdir=None;a.id='k48ap-7B500';a.ipsw='/test.ipsw';a.out=str(d/'out')
    assert device.command(a)[device.command(a).index('--ipsw')+1]=='/test.ipsw'
print('PASS catalog-driven Swift delegation, input translation and retired/conflicting policy rejection')
