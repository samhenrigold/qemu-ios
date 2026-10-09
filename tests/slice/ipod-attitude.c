/* Mounted gravity vectors, compound tilt, invalid input handling, and a turn the gyro sees.
 *
 * SLICE include/hw/arm/ipod-attitude.h file
 * SLICE include/hw/arm/ipod_touch_lis302dl.h typedef LIS302DLState
 * SLICE include/hw/arm/ipod_touch_lis302dl.h define ACCEL_WHOAMI_VALUE|ACCEL_CTRL_REG1_FS
 * SLICE hw/arm/ipod_touch_lis302dl.c define LIS_MOTION_NS_PER_RAD
 * SLICE hw/arm/ipod_touch_lis302dl.c fn lis302dl_counts lis302dl_mounted lis302dl_moving lis302dl_gravity
 * SLICE hw/arm/ipod_touch_lis302dl.c fn lis302dl_motion_rate lis302dl_start_motion lis302dl_apply_attitude
 * SLICE hw/arm/ipod_touch_lis302dl.c fn lis302dl_sample lis302dl_post_load
 */
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#define CLAMP(x, lo, hi) ((x)<(lo)?(lo):(x)>(hi)?(hi):(x))
typedef int I2CSlave; typedef int QEMUTimer;
enum { QEMU_CLOCK_VIRTUAL };
static int64_t clock_ns;
static int64_t qemu_clock_get_ns(int clock) { (void)clock; return clock_ns; }
#include "slice.h"

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
 LIS302DLState sensor={0};
 assert(lis302dl_apply_attitude(&sensor,30,30,true));
 assert(sensor.pitch_mdeg==30000 && sensor.roll_mdeg==30000 && sensor.flat_pose);
 assert(sensor.base_x==-28 && sensor.base_y==32 && sensor.base_z==-48);
 assert(lis302dl_post_load(&sensor,2)==0 && sensor.shake_start_ns == -1);
 sensor.pitch_mdeg=180001;assert(lis302dl_post_load(&sensor,2)==-EINVAL);
 sensor.orientation=3;sensor.base_x=-64;sensor.base_y=sensor.base_z=0;
 sensor.out_x=127;
 assert(!lis302dl_post_load(&sensor,1));
 assert(sensor.pitch_mdeg==0 && sensor.roll_mdeg==90000 && !sensor.flat_pose);
 assert(sensor.out_x==-64 && sensor.shake_start_ns == -1 && sensor.last_sample_ns == -1);
 /* A board mount (N81's DT orientation, inverted): sensor x reads -y, y reads -x. Portrait gravity (0,-64,0). */
 LIS302DLState n81={0};n81.mount=(char *)"-2,-1,3";
 assert(lis302dl_apply_attitude(&n81,0,0,false));
 assert(n81.base_x==64 && n81.base_y==0 && n81.base_z==0);
 assert(lis302dl_apply_attitude(&n81,0,90,false) && n81.base_x==0 && n81.base_y==64);

 /* A board whose gyro sees the turn (motion): Home right to Home left is half a turn about the screen's axis over
  * 700 ms, gravity staying in the screen's plane, at -pi/0.7 rad/s about z; then a quarter turn back to portrait. */
 LIS302DLState turn={0};turn.motion=true;turn.last_sample_ns=turn.shake_start_ns=-1;turn.motion_start_ns=-1;
 double w[3];
 clock_ns=1000000000;
 assert(lis302dl_apply_attitude(&turn,0,90,false));      /* Home right */
 clock_ns=5000000000LL;
 assert(!lis302dl_motion_rate(&turn,clock_ns,w) && w[0]==0 && w[1]==0 && w[2]==0);
 assert(lis302dl_apply_attitude(&turn,0,-90,false));     /* Home left */
 assert(turn.base_x==64 && turn.base_y==0);              /* the steady state is the target at once */
 for (int ms=10;ms<700;ms+=50) {
  turn.last_sample_ns=-1;
  lis302dl_sample(&turn,clock_ns+ms*1000000LL);
  double m=sqrt(turn.out_x*turn.out_x+turn.out_y*turn.out_y);
  assert(abs(turn.out_z)<=1 && fabs(m-64)<3);           /* in the screen's plane, unit gravity */
  assert(lis302dl_motion_rate(&turn,clock_ns+ms*1000000LL,w));
  assert(fabs(w[0])<1e-9 && fabs(w[1])<1e-9 && fabs(fabs(w[2])-M_PI/0.7)<1e-6);
 }
 turn.last_sample_ns=-1;lis302dl_sample(&turn,clock_ns+350000000);
 assert(abs(turn.out_x)<=2 && abs(abs(turn.out_y)-64)<=2);  /* halfway: on end */
 turn.last_sample_ns=-1;lis302dl_sample(&turn,clock_ns+700000000);
 assert(abs(turn.out_x-64)<=1 && abs(turn.out_y)<=1);      /* there */
 assert(!lis302dl_motion_rate(&turn,clock_ns+700000000,w) && w[2]==0);
 clock_ns+=1000000000;
 assert(lis302dl_apply_attitude(&turn,0,0,false));       /* back to portrait: a clockwise quarter turn */
 assert(lis302dl_motion_rate(&turn,clock_ns+100000000,w) && fabs(w[2]-M_PI/0.7)<1e-6);
 turn.last_sample_ns=-1;lis302dl_sample(&turn,clock_ns+175000000);
 assert(turn.out_x>40 && turn.out_y<-40);                  /* between Home left and portrait */
 /* A restore lands where the turn was going. */
 assert(!lis302dl_post_load(&turn,3) && turn.motion_start_ns==-1);
 /* Without a gyro (motion off) an attitude change is still a jump. */
 LIS302DLState jump={0};jump.last_sample_ns=jump.shake_start_ns=-1;
 assert(lis302dl_apply_attitude(&jump,0,90,false) && lis302dl_apply_attitude(&jump,0,-90,false));
 lis302dl_sample(&jump,clock_ns);assert(abs(jump.out_x-64)<=1);
 puts("PASS: mounted portrait/landscape/flat gravity, board mount axes, compound tilt, unit magnitude, invalid input rejection, and a turn the gyro sees");
}
