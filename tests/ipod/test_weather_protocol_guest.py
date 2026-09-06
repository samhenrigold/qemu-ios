#!/usr/bin/env python3
"""Native 7E18 Weather XML acceptance with local synthetic data only."""
import sys,time,tempfile,json,threading,plistlib
from pathlib import Path
from types import SimpleNamespace
from http.server import BaseHTTPRequestHandler,ThreadingHTTPServer
root=Path(__file__).resolve().parents[2];sys.path.insert(0,str(root/'tests/ipod'));import regress as r
out=Path(tempfile.mkdtemp(prefix='it-weather-protocol-'));print('OUTPUT',out,flush=True)
r.START=time.time();files=str(root.parent/'qemu-ios-files')
routing=out/'proxy.conf';requests=[]
class Handler(BaseHTTPRequestHandler):
 def capture(self):
  length=int(self.headers.get('Content-Length','0'))
  if length>1024*1024:self.send_error(413);return
  body=self.rfile.read(length)
  from urllib.parse import urlsplit,parse_qs
  target=urlsplit(self.path)
  # Never retain the gateway's per-install UUID or request headers.
  requests.append(dict(method=self.command,host=target.hostname,path=target.path,service=parse_qs(target.query).get('apptype',[]),body=body.decode('utf-8','replace')))
  (out/'requests.json').write_text(json.dumps(requests,indent=2))
  response=b'';code=410
  if self.command=='POST' and 'apptype=weather' in self.path:
   import xml.etree.ElementTree as ET
   request=ET.fromstring(body);response_root=ET.Element('response');listing=ET.SubElement(ET.SubElement(response_root,'result'),'list')
   for location in request.findall('./query/list/id'):
    item=ET.SubElement(listing,'item')
    ET.SubElement(item,'location',id=location.text,city='Fixture City')
    ET.SubElement(item,'units',temperature='F')
    ET.SubElement(item,'astronomy',sunrise='06:00',sunset='20:00',moonfacevisible='0.5',moonphase='0')
    ET.SubElement(item,'condition',time='14:30',temp='73',code='32')
    for day in range(6):ET.SubElement(item,'forecast',high=str(80+day),low=str(60+day),code='32',dayofweek=str(day+1))
   response=ET.tostring(response_root,encoding='utf-8',xml_declaration=True);code=200
  self.send_response(code);self.send_header('Content-Type','text/xml');self.send_header('Content-Length',str(len(response)));self.end_headers();self.wfile.write(response)
 do_GET=capture;do_POST=capture;do_CONNECT=capture
 def log_message(self,*args):pass
server=ThreadingHTTPServer(('127.0.0.1',0),Handler)
threading.Thread(target=server.serve_forever,daemon=True).start()
routing.write_text('upstream\n127.0.0.1\n%d\n'%server.server_port)
cfg=SimpleNamespace(out=str(out),files=files,base_nand=files+'/nand-agent-v4',nor=files+'/ios3/nor_7E18.bin',overlay=str(out/'overlay'),qemu=str(root/'build-native14/qemu-build/qemu-system-arm'),usbmuxd=str(root/'build-native14/build/usbmuxd/src/usbmuxd'),usbmuxd_ok=True,usb_port=r.free_port(1520,1539),mux_port=r.free_port(27400,27419),qmp_port=r.free_port(28200,28219),wifi=True,cpu=None,mem='128M',kernel_console=True,install_timeout=420,proxy_lo=28460,proxy_hi=28479,web_proxy_config=str(routing))
p=r.Procs();d=r.Device(cfg,p,'device')
try:
 d.start();ok,detail,_=d.wait_for_home(180);assert ok,detail
 deadline=time.time()+60
 while not r.itqmp.agent_alive(d.qmp) and time.time()<deadline:time.sleep(1)
 assert r.itqmp.agent_alive(d.qmp),'guest agent unavailable'
 status,data=r.itqmp.agent(d.qmp,'put','/tmp/itproxy 755',(root/'contrib/it-proxy/itproxy').read_bytes());assert status==0,(status,data)
 status,data=r.itqmp.agent(d.qmp,'exec','chmod 755 /tmp/itproxy && /tmp/itproxy on');assert status==0,(status,data)
 control=r.AgentControl(d.qmp);ok,detail=r.unlock(cfg,control,d);assert ok,detail
 time.sleep(8)
 for app in ['com.apple.weather']:
  result=r.springboard(cfg,control,app);assert result.returncode==0,result
  print('LAUNCHED',app,flush=True);time.sleep(25)
  shot=d.qmp.shot(str(out/(app+'.ppm')));r.to_png(shot,str(out/(app+'.png')))
 d.qmp.home()  # Weather persists preferences when it leaves the foreground.
 time.sleep(5)
 status,data=r.itqmp.agent(d.qmp,'get','/var/mobile/Library/Preferences/com.apple.weather.plist');assert status==0,(status,data)
 (out/'weather.plist').write_bytes(data)
 prefs=plistlib.loads(data)
 assert prefs['Cities'] and all(city['Name']=='Fixture City' for city in prefs['Cities']),prefs
 assert any(request['service']==['weather'] for request in requests),requests
 print('PASS: native Weather accepted synthetic city responses; inspect screenshot for forecast rendering',flush=True)
 print('CAPTURED',len(requests),flush=True)
 assert d.powerdown(), 'shutdown not confirmed'
finally:
 if d.qmp:d.qmp.close()
 p.stop_all();server.shutdown();server.server_close()
