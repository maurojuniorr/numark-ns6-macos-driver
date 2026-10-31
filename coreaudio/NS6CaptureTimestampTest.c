#include "NS6CaptureTimestamp.h"
#include <assert.h>
int main(void) {
    NS6CaptureTimestamp a={1000,1000000000}, b={1441,1010000000};
    uint64_t t=0;
    assert(ns6_capture_timestamp_interpolate(a,b,1220,&t));
    assert(t==1004988662);
    assert(ns6_capture_timestamp_interpolate(a,b,1000,&t) && t==1000000000);
    assert(ns6_capture_timestamp_interpolate(a,b,1441,&t) && t==1010000000);
    assert(!ns6_capture_timestamp_interpolate(a,b,999,&t));
    assert(!ns6_capture_timestamp_interpolate(b,a,1220,&t));
    assert(!ns6_capture_timestamp_interpolate(a,b,1220,0));
    NS6CaptureTimestamp c={0,0}, d={UINT64_MAX,UINT64_MAX};
    assert(ns6_capture_timestamp_interpolate(c,d,UINT64_MAX,&t) && t==UINT64_MAX);
    assert(!ns6_capture_timestamp_interpolate(a,a,1000,&t));
    return 0;
}
