#include "NS6CaptureReaders.h"
#include <assert.h>
int main(void) {
    NS6CaptureFanout ring; NS6CaptureReaders readers;
    ns6_capture_fanout_init(&ring); ns6_capture_readers_init(&readers);
    float x[4]={9,9,9,9},y[4]={9,9,9,9};
    assert(ns6_capture_readers_read(&readers,&ring,7,1,1,x,2)==0);
    assert(ns6_capture_readers_read(&readers,&ring,8,1,1,y,2)==0);
    const float samples[]={1,2,3,4}; ns6_capture_fanout_write(&ring,samples,2);
    assert(ns6_capture_readers_read(&readers,&ring,7,1,1,x,2)==2);
    assert(ns6_capture_readers_read(&readers,&ring,8,1,1,y,2)==2);
    for(int i=0;i<4;i++)assert(x[i]==samples[i]&&y[i]==samples[i]);
    ns6_capture_fanout_write(&ring,samples,2);
    assert(ns6_capture_readers_read(&readers,&ring,7,1,2,x,2)==0);
    ns6_capture_readers_remove(&readers,8);
    assert(ns6_capture_readers_read(&readers,&ring,8,1,2,y,2)==0);
    return 0;
}
