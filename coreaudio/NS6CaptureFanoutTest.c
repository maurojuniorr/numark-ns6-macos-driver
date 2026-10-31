#include "NS6CaptureFanout.h"
#include <stdbool.h>
#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sched.h>
static NS6CaptureFanout shared;
static atomic_bool done;
static void *producer(void *unused) {
    (void)unused;
    for(unsigned n=1;n<=100000;n++){float frame[2]={(float)n,-(float)n}; ns6_capture_fanout_write(&shared,frame,1);}
    atomic_store(&done,true);
    return 0;
}
static void *consumer(void *unused) {
    (void)unused; NS6CaptureFanoutReader reader; ns6_capture_fanout_reader_init(&reader,&shared);
    float previous=0;
    do { float frame[2]; if(ns6_capture_fanout_read(&shared,&reader,frame,1)){assert(frame[0]==-frame[1]);assert(frame[0]>previous);previous=frame[0];} else sched_yield(); } while(!atomic_load(&done));
    return 0;
}
int main(void) {
    NS6CaptureFanout ring;
    NS6CaptureFanoutReader a,b;
    ns6_capture_fanout_init(&ring);
    ns6_capture_fanout_reader_init(&a,&ring);
    ns6_capture_fanout_reader_init(&b,&ring);
    const float src[]={1,2,3,4,5,6,7,8}; float x[8],y[8];
    assert(ns6_capture_fanout_write(&ring,src,4)==4);
    assert(ns6_capture_fanout_read(&ring,&a,x,4)==4);
    assert(ns6_capture_fanout_read(&ring,&b,y,4)==4);
    for(unsigned i=0;i<8;i++)assert(x[i]==src[i]&&y[i]==src[i]);
    float many[NS6_FANOUT_FRAMES*2];
    for(unsigned i=0;i<NS6_FANOUT_FRAMES;i++){many[2*i]=(float)i;many[2*i+1]=-(float)i;}
    assert(ns6_capture_fanout_write(&ring,many,NS6_FANOUT_FRAMES)==NS6_FANOUT_FRAMES);
    assert(ns6_capture_fanout_read(&ring,&a,x,4)==4);
    assert(ns6_capture_fanout_read(&ring,&b,y,4)==4);
    for(unsigned i=0;i<8;i++)assert(x[i]==y[i]&&x[i]==many[i]);
    NS6CaptureFanoutReader slow; ns6_capture_fanout_reader_init(&slow,&ring);
    ns6_capture_fanout_write(&ring,src,4);
    ns6_capture_fanout_write(&ring,many,NS6_FANOUT_FRAMES);
    assert(ns6_capture_fanout_read(&ring,&slow,x,4)==4);
    assert(slow.dropped==4);
    for(unsigned i=0;i<8;i++)assert(x[i]==many[i]);
    ns6_capture_fanout_init(&shared); atomic_init(&done,false);
    pthread_t w,r,r2; assert(pthread_create(&w,0,producer,0)==0);
    assert(pthread_create(&r,0,consumer,0)==0); assert(pthread_create(&r2,0,consumer,0)==0);
    assert(pthread_join(w,0)==0); assert(pthread_join(r,0)==0); assert(pthread_join(r2,0)==0);
    return 0;
}
