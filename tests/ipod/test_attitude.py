#!/usr/bin/env python3
"""Mounted gravity vectors, compound tilt and invalid input handling."""
from pathlib import Path
import re
import subprocess
import tempfile
root=Path(__file__).resolve().parents[2]
code=r'''
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include "hw/arm/ipod-attitude.h"
static void check(double p,double r,bool flat,int x,int y,int z) {
 int8_t actual[3];assert(ipod_attitude_vector(p,r,flat,actual));
 assert(actual[0]==x&&actual[1]==y&&actual[2]==z);
}
int main(void) {
 check(0,0,false,0,-64,0);check(0,180,false,0,64,0);
 check(0,90,false,-64,0,0);check(0,-90,false,64,0,0);
 check(0,0,true,0,0,-64);check(0,180,true,0,0,64);
 check(90,0,false,0,0,-64);check(90,0,true,0,64,0);
 check(30,30,false,-28,-48,-32);check(30,30,true,-28,32,-48);
 for(int flat=0;flat<2;flat++)for(int p=-180;p<=180;p+=3)for(int r=-180;r<=180;r+=3) {
  int8_t v[3];assert(ipod_attitude_vector(p,r,flat,v));
  double magnitude=sqrt(v[0]*v[0]+v[1]*v[1]+v[2]*v[2]);
  assert(fabs(magnitude-64)<1);
 }
 int8_t v[3]={11,22,33},original[3];memcpy(original,v,3);
 assert(!ipod_attitude_vector(NAN,0,false,v));
 assert(!ipod_attitude_vector(0,INFINITY,false,v));
 assert(!ipod_attitude_vector(181,0,false,v));
 assert(!ipod_attitude_vector(0,-181,false,v));
 assert(!memcmp(v,original,3));
 puts("PASS: mounted portrait/landscape/flat gravity, compound tilt, unit magnitude and invalid input rejection");
}
'''
source=(root/'hw/arm/ipod_touch_lis302dl.c').read_text()
header=(root/'include/hw/arm/ipod_touch_lis302dl.h').read_text()
state=re.search(r'typedef struct LIS302DLState \{.*?\n} LIS302DLState;',header,re.S).group()
functions='\n'.join(re.search(r'^(?:static )?[^\n]*\b'+name+r'\([^;]*?\n\{.*?^}',source,re.M|re.S).group()
                    for name in ('lis302dl_apply_attitude','lis302dl_post_load'))
extra = '#include <errno.h>\ntypedef int I2CSlave; typedef int QEMUTimer;\nstatic void timer_del(QEMUTimer *timer) { *timer=0; }\n' + state + '\n' + functions + '\n'
code=code.replace('static void check(',extra+'static void check(')
code=code.replace(' puts("PASS:',r''' int timer=1; LIS302DLState sensor={.shake_timer=&timer};
 assert(lis302dl_apply_attitude(&sensor,30,30,true));
 assert(sensor.pitch_mdeg==30000 && sensor.roll_mdeg==30000 && sensor.flat_pose);
 assert(sensor.base_x==-28 && sensor.base_y==32 && sensor.base_z==-48);
 assert(lis302dl_post_load(&sensor,2)==0 && !timer);
 sensor.pitch_mdeg=180001;assert(lis302dl_post_load(&sensor,2)==-EINVAL);
 sensor.orientation=3;sensor.base_x=-64;sensor.base_y=sensor.base_z=0;
 sensor.out_x=127;timer=1;
 assert(!lis302dl_post_load(&sensor,1));
 assert(sensor.pitch_mdeg==0 && sensor.roll_mdeg==90000 && !sensor.flat_pose);
 assert(sensor.out_x==-64 && !timer && !sensor.shake_ticks);
 puts("PASS:''')
with tempfile.TemporaryDirectory() as directory:
    path=Path(directory)/'check.c';path.write_text(code)
    subprocess.run(['clang','-fsanitize=address,undefined','-fno-sanitize-recover=all','-I'+str(root/'include'),str(path),'-o',directory+'/check'],check=True)
    subprocess.run([directory+'/check'],check=True)
