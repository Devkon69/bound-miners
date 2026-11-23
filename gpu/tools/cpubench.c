/* Velocita' di BoundHash sulla CPU e confronto col riferimento:  cpubench THREAD HASH_PER_THREAD  (BOUND_CPU_SIMD=0|1 per forzare scalare / AVX2) */
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <pthread.h>
#include "bound_pow.h"
#include "cpu_hash.h"
static double now(){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return t.tv_sec+t.tv_nsec*1e-9;}
static volatile int stop; static long cnt[64]; static int LANES;
static void *w(void *p){long id=(long)p;cpuh_t h;cpuh_init(&h,4096,LANES);uint8_t hd[80]={1},o[4][32];cpuh_job(&h,hd);uint32_t n[4]={id<<24,(id<<24)+1,(id<<24)+2,(id<<24)+3};
 while(!stop){cpuh_hashn(&h,n,o);for(int i=0;i<4;i++)n[i]+=4;cnt[id]+=LANES;}return 0;}
int main(int c,char**v){const char*e=cpuh_engine();bp_set_lines(64);int s64=cpuh_selftest(64);bp_set_lines(4096);int s4k=cpuh_selftest(4096);
 int T=c>1?atoi(v[1]):1;LANES=c>2?atoi(v[2]):1;printf("%s selftest %d/%d  ",e,s4k,s64);pthread_t th[64];double t0=now();for(long i=0;i<T;i++)pthread_create(&th[i],0,w,(void*)i);
 while(now()-t0<3);stop=1;long s=0;for(int i=0;i<T;i++){pthread_join(th[i],0);s+=cnt[i];}printf("%d thread x %d: %.0f H/s\n",T,LANES,s/(now()-t0));}
