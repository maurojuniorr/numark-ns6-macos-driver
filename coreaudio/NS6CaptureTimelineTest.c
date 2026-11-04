#include "NS6CaptureTimeline.h"
#include <assert.h>

int main(void) {
    static NS6CaptureFanout ring;
    static NS6CaptureTimeline timeline;
    static float source[4096 * 2];
    float a[8], b[8];
    ns6_capture_fanout_init(&ring);
    ns6_capture_timeline_init(&timeline);
    for (unsigned i = 0; i < 4096; ++i) source[2*i] = source[2*i+1] = (float)(i+1);
    assert(ns6_capture_timeline_read(&timeline, &ring, 1, 1, 100, true, a, 4) == 0);
    ns6_capture_fanout_write(&ring, source, 2048);
    assert(ns6_capture_timeline_read(&timeline, &ring, 1, 1, 104, true, a, 4) == 4);
    for (unsigned i = 0; i < 4; ++i) assert(a[2*i] == source[2*i]);
    assert(ns6_capture_timeline_read(&timeline, &ring, 1, 1, 104, true, b, 4) == 4);
    for (unsigned i = 0; i < 8; ++i) assert(a[i] == b[i]);
    assert(ns6_capture_timeline_read(&timeline, &ring, 1, 1, 108, true, b, 4) == 4);
    assert(b[0] > a[6]);
    assert(ns6_capture_timeline_read(&timeline, &ring, 1, 1, 500, true, b, 4) == 0);
    for (unsigned i = 0; i < 8; ++i) assert(b[i] == 0);
    assert(ns6_capture_timeline_read(&timeline, &ring, 1, 1, 504, false, b, 4) == 0);
    ns6_capture_fanout_init(&ring);
    ns6_capture_timeline_init(&timeline);
    static float block[256*2], output[256*2];
    for (unsigned i = 0; i < 256*2; ++i) block[i] = 0.5f;
    assert(ns6_capture_timeline_read(&timeline, &ring, 2, 1, 0, true, output, 256) == 0);
    for (unsigned i = 0; i < 8; ++i) ns6_capture_fanout_write(&ring, block, 256);
    for (unsigned cycle = 1; cycle < 3000; ++cycle) {
        assert(ns6_capture_timeline_read(&timeline, &ring, 2, 1, cycle*256, true, output, 256) == 256);
        for (unsigned i = 0; i < 256*2; ++i) assert(output[i] == 0.5f);
        ns6_capture_fanout_write(&ring, block, 256);
        if (cycle % 20 == 0) ns6_capture_fanout_write(&ring, block, 1);
    }
    return 0;
}
