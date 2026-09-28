#!/usr/bin/env python3
"""Read baked guest files through their HFS catalog and verify bytes/ownership."""
import argparse, importlib.util, plistlib, struct, sys
from pathlib import Path
root=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(root/'imgtools'))
import hfsvol, setowner
parser=argparse.ArgumentParser(description=__doc__)
parser.add_argument('--nand',required=True)
parser.add_argument('--fresh',action='store_true',help='expect builder preference modes before guest runtime writes')
parser.add_argument('--without-guest-tools',action='store_true',help='verify a legacy stock/software-CA bake')
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
 ('build/it-boot/armv6/it_boot','/usr/local/bin/it_boot',0o755),
 ('contrib/it-boot/com.qemu.it-boot.plist','/System/Library/LaunchDaemons/com.qemu.it-boot.plist',0o644),
):
 if args.without_guest_tools:
  if source.endswith('/MBXGLEngine') or '/it-boot/' in source:  # the loader is baked on every build
   continue
  try:setowner.resolve(index,target)
  except SystemExit:continue
  raise AssertionError('unsupported helper installed: '+target)
 assert read(target,mode=mode)==(root/source).read_bytes(),target+' is stale'
 print('CURRENT',target)
job=plistlib.loads(read('/System/Library/LaunchDaemons/com.apple.SpringBoard.plist',mode=0o644))
assert job['Label']=='com.apple.SpringBoard'
env=job['EnvironmentVariables']
expected_ogl='0' if args.without_guest_tools else '1'
assert env['CA_ENABLE_OGL']==expected_ogl and env['LK_ENABLE_OGL']==expected_ogl
libraries=env.get('DYLD_INSERT_LIBRARIES','').split(':')
assert ('/usr/lib/it_typein.dylib' in libraries)==(not args.without_guest_tools)
assert '/usr/lib/it_kbd_agent.dylib' not in libraries
for version in (1,2):
 path='/private/var/mobile/Media/.lt-guest-tools-v%d'%version
 if args.without_guest_tools:
  try:setowner.resolve(index,path)
  except SystemExit:pass
  else:raise AssertionError('unsupported helper capability advertised: '+path)
 else:assert read(path,501,501,0o644)==('v%d\n'%version).encode()
for job in ('com.qemu.it-pbd.plist','com.qemu.it-agent.plist'):  # legacy clipboard daemon; the agent is the seed package's
 try:setowner.resolve(index,'/System/Library/LaunchDaemons/'+job)
 except SystemExit:pass
 else:raise AssertionError('baked job remains: '+job)
if not args.without_guest_tools:
 seed=read('/usr/local/lighttouch/state',mode=0o644).decode().split()
 assert seed[0]=='seed' and b'job ' in read('/usr/local/lighttouch/pkgs/%s/offer'%seed[1],mode=0o644),seed
spec=importlib.util.spec_from_file_location('sound_defaults',root/'imgtools/set-sound-defaults.py')
sounds=importlib.util.module_from_spec(spec);spec.loader.exec_module(sounds)
for name,expected in sounds.DEFAULTS.items():
 prefs=plistlib.loads(read('/private/var/mobile/Library/Preferences/'+name,501,501,0o644 if args.fresh else 0o600))
 assert all(prefs.get(k)==v for k,v in expected.items()),name
 if name=='com.apple.springboard.plist':
  assert 'SBDontLockEver' not in prefs and 'SBDisableCABlanking' not in prefs
print('PASS: current guest components, guest ownership, launch environment, lock and all five sound defaults')
