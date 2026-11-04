#ifndef NS6_CAPTURE_TIMELINE_H
#define NS6_CAPTURE_TIMELINE_H
#include "NS6CaptureFanout.h"
#include <stdbool.h>
#include <math.h>

enum { NS6_CAPTURE_RESERVE = 2048, NS6_CAPTURE_TIMELINE_FRAMES = 8192 };
typedef struct {
    atomic_flag busy;
    uint64_t session, epoch, source, next, first;
    double fraction;
    bool anchored, primed;
    float samples[NS6_CAPTURE_TIMELINE_FRAMES * 2];
} NS6CaptureTimeline;

static inline void ns6_capture_timeline_init(NS6CaptureTimeline *t) {
    memset(t, 0, sizeof(*t));
    atomic_flag_clear(&t->busy);
}

static inline bool ns6_capture_timeline_sample(const NS6CaptureFanout *ring,
                                                uint64_t position, float out[2]) {
    const NS6CaptureFanoutSlot *s = &ring->slots[position % NS6_FANOUT_FRAMES];
    uint64_t sequence = atomic_load_explicit(&s->sequence, memory_order_seq_cst);
    uint32_t left = atomic_load_explicit(&s->left, memory_order_seq_cst);
    uint32_t right = atomic_load_explicit(&s->right, memory_order_seq_cst);
    if (sequence != position + 1 || sequence != atomic_load_explicit(&s->sequence, memory_order_seq_cst)) return false;
    memcpy(&out[0], &left, sizeof(left)); memcpy(&out[1], &right, sizeof(right));
    return true;
}

static inline size_t ns6_capture_timeline_read(NS6CaptureTimeline *t, const NS6CaptureFanout *ring,
        uint64_t session, uint64_t epoch, uint64_t sample, bool valid, float *out, size_t frames) {
    memset(out, 0, frames * 2 * sizeof(float));
    if (!valid || frames > NS6_CAPTURE_TIMELINE_FRAMES || sample > UINT64_MAX - frames ||
        atomic_flag_test_and_set_explicit(&t->busy, memory_order_acquire)) return 0;
    uint64_t head = atomic_load_explicit(&ring->head, memory_order_seq_cst);
    if (!t->anchored || t->session != session || t->epoch != epoch || sample > t->next) {
        t->session = session; t->epoch = epoch; t->source = head;
        t->next = t->first = sample; t->fraction = 0; t->primed = false; t->anchored = true;
    }
    size_t copied = 0;
    uint64_t queued = head - t->source;
    if (queued > NS6_FANOUT_FRAMES) {
        t->source = head; t->fraction = 0; t->primed = false; queued = 0;
    }
    if (!t->primed && queued >= NS6_CAPTURE_RESERVE) t->primed = true;
    double step = 1.0 + ((double)queued - NS6_CAPTURE_RESERVE) * 0.000001;
    if (step < 0.998) step = 0.998;
    if (step > 1.002) step = 1.002;
    for (size_t n = 0; n < frames; ++n) {
        uint64_t pos = sample + n;
        if (pos < t->first) continue;
        if (pos < t->next) {
            size_t i = (size_t)(pos % NS6_CAPTURE_TIMELINE_FRAMES) * 2;
            out[2*n] = t->samples[i]; out[2*n+1] = t->samples[i+1]; ++copied;
            continue;
        }
        float a[2], b[2];
        if (!t->primed || t->source >= head ||
            !ns6_capture_timeline_sample(ring, t->source, a)) {
            size_t i = (size_t)(pos % NS6_CAPTURE_TIMELINE_FRAMES) * 2;
            t->samples[i] = t->samples[i+1] = 0;
            t->next = pos + 1;
            if (t->next - t->first > NS6_CAPTURE_TIMELINE_FRAMES) t->first = t->next - NS6_CAPTURE_TIMELINE_FRAMES;
            continue;
        }
        if (t->fraction > 0 && t->source + 1 < head) {
            if (!ns6_capture_timeline_sample(ring, t->source + 1, b)) break;
        } else { b[0] = a[0]; b[1] = a[1]; }
        size_t i = (size_t)(pos % NS6_CAPTURE_TIMELINE_FRAMES) * 2;
        for (unsigned c = 0; c < 2; ++c)
            out[2*n+c] = t->samples[i+c] = a[c] + (float)t->fraction * (b[c] - a[c]);
        t->fraction += step;
        uint64_t advance = (uint64_t)t->fraction;
        t->source += advance; t->fraction -= (double)advance;
        t->next = pos + 1;
        if (t->next - t->first > NS6_CAPTURE_TIMELINE_FRAMES) t->first = t->next - NS6_CAPTURE_TIMELINE_FRAMES;
        ++copied;
    }
    atomic_flag_clear_explicit(&t->busy, memory_order_release);
    return copied;
}
#endif
