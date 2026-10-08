/* Actual Darwin poll wait: native descriptor/EINTR parity with GLib, sanitizer
 * coverage and observed deadline latency. Timings are evidence, not a load-dependent
 * pass threshold. Unsupported masks, zero/infinite waits preserve GLib's path.
 * The native wait must receive the requested nanoseconds, not a millisecond-rounded timeout
 * (observed_timeout.tv_nsec==100000).
 *
 * SLICE:rounder util/qemu-timer.c fn qemu_timeout_ns_to_ms
 * SLICE:darwin util/qemu-timer.c fn qemu_poll_ns_darwin
 * PKG glib-2.0
 */
#include <glib.h>
#include <sys/event.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <poll.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <assert.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <time.h>
#define SCALE_MS 1000000
#define NANOSECONDS_PER_SECOND 1000000000LL
#define DIV_ROUND_UP(n,d) (((n)+(d)-1)/(d))
#include "rounder.h"
static struct timespec observed_timeout;
static unsigned kevent_calls;
static int observed_kevent(int queue, const struct kevent *changes, int count,
                           struct kevent *events, int nevents, const struct timespec *ts) {
 assert(ts);observed_timeout=*ts;kevent_calls++;
 return kevent(queue,changes,count,events,nevents,ts);
}
#define kevent(...) observed_kevent(__VA_ARGS__)
#include "darwin.h"
#undef kevent
static int peer;
static void *writer(void *x) { struct timespec t={0,300000};nanosleep(&t,0);assert(write(peer,"x",1)==1);return 0; }
static void *closer(void *x) { struct timespec t={0,300000};nanosleep(&t,0);close(peer);return 0; }
static void sig(int n) {}
static void *signaler(void *x) { struct timespec t={0,300000};nanosleep(&t,0);pthread_kill(*(pthread_t*)x,SIGUSR1);return 0; }
static long long stamp(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (long long)t.tv_sec*1000000000+t.tv_nsec; }
int main(void) {
 int p[2];pthread_t th;assert(!pipe(p));peer=p[1];
 GPollFD fds[3]={{p[0],G_IO_IN,0},{p[0],G_IO_IN,0},{-1,G_IO_IN,0}};
 assert(!qemu_poll_ns_darwin(fds,3,100000));assert(!fds[0].revents&&!fds[2].revents);
 assert(kevent_calls==1 && observed_timeout.tv_sec==0 && observed_timeout.tv_nsec==100000);
 assert(!pthread_create(&th,0,writer,0));assert(qemu_poll_ns_darwin(fds,3,20000000)==1);pthread_join(th,0);
 assert(fds[0].revents==G_IO_IN&&fds[1].revents==G_IO_IN&&!fds[2].revents);
 char b;assert(read(p[0],&b,1)==1);
 assert(!pthread_create(&th,0,closer,0));assert(qemu_poll_ns_darwin(fds,3,20000000)==1);pthread_join(th,0);GPollFD expected={p[0],G_IO_IN,0};assert(g_poll(&expected,1,0)==1);assert(fds[0].revents==expected.revents);close(p[0]);
 GPollFD bad={p[0],G_IO_IN,0};GPollFD expected_bad=bad;int expected_rc=g_poll(&expected_bad,1,0);assert(qemu_poll_ns_darwin(&bad,1,300000)==expected_rc);assert(bad.revents==expected_bad.revents);
 int sp[2];assert(!socketpair(AF_UNIX,SOCK_STREAM,0,sp));GPollFD out={sp[0],G_IO_OUT,0};assert(qemu_poll_ns_darwin(&out,1,300000)==1&&out.revents&G_IO_OUT);close(sp[0]);close(sp[1]);
 struct sigaction a={0};a.sa_handler=sig;sigemptyset(&a.sa_mask);assert(!sigaction(SIGUSR1,&a,0));pthread_t self=pthread_self();assert(!pthread_create(&th,0,signaler,&self));assert(qemu_poll_ns_darwin(0,0,20000000)==-1&&errno==EINTR);pthread_join(th,0);
 /* Zero/infinite/unsupported masks stay on poll; readiness remains identical. */
 assert(!pipe(p));assert(write(p[1],"x",1)==1);GPollFD unknown={p[0],G_IO_IN|G_IO_PRI,0};assert(qemu_poll_ns_darwin(&unknown,1,-1)==1&&unknown.revents==G_IO_IN);assert(qemu_poll_ns_darwin(&unknown,1,0)==1);close(p[0]);close(p[1]);
 long long old=0,new=0;for(int i=0;i<100;i++){long long t=stamp();g_poll(0,0,1);old+=stamp()-t;t=stamp();assert(!qemu_poll_ns_darwin(0,0,100000));new+=stamp()-t;}
 printf("PASS native pipe/socket/duplicate/negative/invalid/HUP/EINTR/zero/infinite/unsupported; wait100us baseline=%lldns candidate=%lldns\n",old/100,new/100);
}
