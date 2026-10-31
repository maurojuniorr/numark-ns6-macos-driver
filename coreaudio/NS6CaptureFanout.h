#ifndef NS6_CAPTURE_FANOUT_H
#define NS6_CAPTURE_FANOUT_H
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

enum { NS6_FANOUT_FRAMES=8192 };
typedef struct { atomic_uint_fast64_t sequence; atomic_uint left, right; } NS6CaptureFanoutSlot;
typedef struct { NS6CaptureFanoutSlot slots[NS6_FANOUT_FRAMES]; atomic_uint_fast64_t head; } NS6CaptureFanout;
typedef struct { uint64_t cursor, dropped; } NS6CaptureFanoutReader;
static inline void ns6_capture_fanout_init(NS6CaptureFanout *ring) {
    atomic_init(&ring->head,0);
    for(size_t i=0;i<NS6_FANOUT_FRAMES;i++) {
        atomic_init(&ring->slots[i].sequence,0);
        atomic_init(&ring->slots[i].left,0);
        atomic_init(&ring->slots[i].right,0);
    }
}
static inline void ns6_capture_fanout_reader_init(NS6CaptureFanoutReader *reader,const NS6CaptureFanout *ring) {
    reader->cursor=atomic_load_explicit(&ring->head,memory_order_seq_cst);
    reader->dropped=0;
}
static inline size_t ns6_capture_fanout_write(NS6CaptureFanout *ring,const float *samples,size_t frames) {
    uint64_t head=atomic_load_explicit(&ring->head,memory_order_seq_cst);
    for(size_t i=0;i<frames;i++) {
        NS6CaptureFanoutSlot *slot=&ring->slots[(head+i)%NS6_FANOUT_FRAMES];
        uint32_t left,right; memcpy(&left,&samples[2*i],sizeof(left)); memcpy(&right,&samples[2*i+1],sizeof(right));
        atomic_store_explicit(&slot->sequence,0,memory_order_seq_cst);
        atomic_store_explicit(&slot->left,left,memory_order_seq_cst);
        atomic_store_explicit(&slot->right,right,memory_order_seq_cst);
        atomic_store_explicit(&slot->sequence,head+i+1,memory_order_seq_cst);
    }
    atomic_store_explicit(&ring->head,head+frames,memory_order_seq_cst);
    return frames;
}
static inline size_t ns6_capture_fanout_read(const NS6CaptureFanout *ring,NS6CaptureFanoutReader *reader,float *out,size_t frames) {
    uint64_t head=atomic_load_explicit(&ring->head,memory_order_seq_cst);
    if(head-reader->cursor>NS6_FANOUT_FRAMES){reader->dropped+=head-reader->cursor-NS6_FANOUT_FRAMES;reader->cursor=head-NS6_FANOUT_FRAMES;}
    size_t copied=0;
    while(copied<frames && reader->cursor<head) {
        const NS6CaptureFanoutSlot *slot=&ring->slots[reader->cursor%NS6_FANOUT_FRAMES];
        uint64_t seq=atomic_load_explicit(&slot->sequence,memory_order_seq_cst);
        uint32_t left=atomic_load_explicit(&slot->left,memory_order_seq_cst),right=atomic_load_explicit(&slot->right,memory_order_seq_cst);
        if(seq!=reader->cursor+1 || seq!=atomic_load_explicit(&slot->sequence,memory_order_seq_cst)) break;
        memcpy(&out[2*copied],&left,sizeof(left));memcpy(&out[2*copied+1],&right,sizeof(right));
        ++copied;++reader->cursor;
    }
    for(size_t i=copied;i<frames;i++)out[2*i]=out[2*i+1]=0;
    return copied;
}
#endif
