#ifndef NS6_CAPTURE_TIMESTAMP_H
#define NS6_CAPTURE_TIMESTAMP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct { uint64_t frame; uint64_t host_ns; } NS6CaptureTimestamp;

static inline bool ns6_capture_timestamp_interpolate(NS6CaptureTimestamp a,
        NS6CaptureTimestamp b, uint64_t frame, uint64_t *host_ns) {
    if (!host_ns || b.frame <= a.frame || b.host_ns < a.host_ns ||
        frame < a.frame || frame > b.frame) return false;
    uint64_t frames = b.frame - a.frame;
    uint64_t elapsed = b.host_ns - a.host_ns;
    uint64_t offset = frame - a.frame;
    __uint128_t interpolated = (__uint128_t)elapsed * offset / frames + a.host_ns;
    if (interpolated > UINT64_MAX) return false;
    *host_ns = (uint64_t)interpolated;
    return true;
}
#endif
