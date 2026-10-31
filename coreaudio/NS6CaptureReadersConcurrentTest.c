#include "NS6CaptureReaders.h"
#include <assert.h>
#include <pthread.h>
#include <sched.h>
static NS6CaptureFanout ring;
static NS6CaptureReaders readers;
static atomic_bool done;
static void *produce(void *unused) {
    (void)unused;
    for(unsigned n=1;n<=100000;n++){float p[2]={(float)n,-(float)n};ns6_capture_fanout_write(&ring,p,1);}
    atomic_store(&done,true);return NULL;
}
static void *consume(void *arg) {
    uint32_t id=(uint32_t)(uintptr_t)arg; float previous=0;
    do {float p[2];size_t n=ns6_capture_readers_read(&readers,&ring,id,9,1,p,1);
        if(n){assert(p[0]==-p[1]);assert(p[0]>previous);previous=p[0];}else sched_yield();
    }while(!atomic_load(&done));
    return NULL;
}
static void *remove_reader(void *unused) {
    (void)unused;
    while(!atomic_load(&done)){ns6_capture_readers_remove(&readers,2);sched_yield();}
    return NULL;
}
int main(void) {
    ns6_capture_fanout_init(&ring);ns6_capture_readers_init(&readers);atomic_init(&done,false);
    pthread_t p,a,b,c;
    assert(!pthread_create(&p,NULL,produce,NULL));assert(!pthread_create(&a,NULL,consume,(void*)(uintptr_t)1));
    assert(!pthread_create(&b,NULL,consume,(void*)(uintptr_t)2));assert(!pthread_create(&c,NULL,remove_reader,NULL));
    assert(!pthread_join(p,NULL));assert(!pthread_join(a,NULL));assert(!pthread_join(b,NULL));assert(!pthread_join(c,NULL));
    return 0;
}
