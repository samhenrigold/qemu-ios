#!/usr/bin/env python3
"""Read baked guest files through their HFS catalog and verify bytes/ownership."""
import argparse, importlib.util, plistlib, struct, sys
from pathlib import Path
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'imgtools'))
import hfsvol, setowner
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--nand',required=True)
args=parser.parse_args()
volume=hfsvol.Volume(args.nand)
catalog=hfsvol.BTree(volume.catalog)
index=setowner.index_catalog(catalog)
def read(path,uid=0,gid=0,mode=0o755):
 node,offset,kind,_=setowner.resolve(index,path)
 assert kind==hfsvol.kHFSPlusFileRecord,path
 record=catalog.node(node)
 owner,group=struct.unpack_from('>II',record,offset+32)
 actual_mode=struct.unpack_from('>H',record,offset+42)[0]&0o7777
 assert (owner,group,actual_mode)==(uid,gid,mode),(path,owner,group,oct(actual_mode))
 fork=hfsvol.Fork(volume,record,offset+88)
 data=fork.read(0,fork.logical_size)
 assert len(data)==fork.logical_size,(path,'unsupported or truncated fork')
 return data
for source,target,mode in (
 ('contrib/it-gles/MBXGLEngine','/System/Library/Frameworks/OpenGLES.framework/MBXGLEngine.bundle/MBXGLEngine',0o755),
 ('contrib/it-gles/sblaunch','/usr/local/bin/sblaunch',0o755),
 ('contrib/it-instprogress/sbdlicon','/usr/local/bin/sbdlicon',0o755),
 ('contrib/it-agent/it_agent','/usr/local/bin/it_agent',0o755),
 ('contrib/it-agent/it_typein.dylib','/usr/lib/it_typein.dylib',0o755),
 ('contrib/it-agent/com.qemu.it-agent.plist','/System/Library/LaunchDaemons/com.qemu.it-agent.plist',0o644),
):
 assert read(target,mode=mode)==(root/source).read_bytes(),target+' is stale'
 print('CURRENT',target)
job=plistlib.loads(read('/System/Library/LaunchDaemons/com.apple.SpringBoard.plist',mode=0o644))
assert job['Label']=='com.apple.SpringBoard'
env=job['EnvironmentVariables']
assert env['CA_ENABLE_OGL']=='1' and env['LK_ENABLE_OGL']=='1'
assert '/usr/lib/it_typein.dylib' in env['DYLD_INSERT_LIBRARIES'].split(':')
assert '/usr/lib/it_kbd_agent.dylib' not in env['DYLD_INSERT_LIBRARIES'].split(':')
try:setowner.resolve(index,'/System/Library/LaunchDaemons/com.qemu.it-pbd.plist')
except SystemExit:pass
else:raise AssertionError('legacy clipboard daemon remains')
spec=importlib.util.spec_from_file_location('sound_defaults',root/'imgtools/set-sound-defaults.py')
sounds=importlib.util.module_from_spec(spec);spec.loader.exec_module(sounds)
for name,expected in sounds.DEFAULTS.items():
 prefs=plistlib.loads(read('/private/var/mobile/Library/Preferences/'+name,501,501,0o600))
 assert all(prefs.get(k)==v for k,v in expected.items()),name
 if name=='com.apple.springboard.plist':
  assert 'SBDontLockEver' not in prefs and 'SBDisableCABlanking' not in prefs
print('PASS: current guest components, guest ownership, launch environment, lock and all five sound defaults')
