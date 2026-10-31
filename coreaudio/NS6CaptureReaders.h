#ifndef NS6_CAPTURE_READERS_H
#define NS6_CAPTURE_READERS_H
#include "NS6CaptureFanout.h"
#include <stdbool.h>
#include <sched.h>

enum { NS6_CAPTURE_READERS=128 };
typedef struct {
    atomic_uint_fast64_t key, dropped;
    atomic_bool busy, retired;
    uint64_t session,epoch;
    NS6CaptureFanoutReader reader;
} NS6CaptureReaderSlot;
typedef struct { NS6CaptureReaderSlot slots[NS6_CAPTURE_READERS]; atomic_bool registry_busy; } NS6CaptureReaders;
static inline void ns6_capture_readers_init(NS6CaptureReaders *readers) {
    atomic_init(&readers->registry_busy,false);
    for(size_t i=0;i<NS6_CAPTURE_READERS;i++){
        atomic_init(&readers->slots[i].key,0); atomic_init(&readers->slots[i].dropped,0); atomic_init(&readers->slots[i].busy,false);
        atomic_init(&readers->slots[i].retired,false); readers->slots[i].session=readers->slots[i].epoch=0;
    }
}
static inline size_t ns6_capture_readers_read(NS6CaptureReaders *readers,const NS6CaptureFanout *ring,
        uint32_t client,uint64_t session,uint64_t epoch,float *out,size_t frames) {
    uint64_t key=(uint64_t)client+1; NS6CaptureReaderSlot *slot=NULL;
    for(size_t i=0;i<NS6_CAPTURE_READERS;i++)if(atomic_load(&readers->slots[i].key)==key){slot=&readers->slots[i];break;}
    if(!slot){
        bool registry_expected=false;
        if(atomic_compare_exchange_strong(&readers->registry_busy,&registry_expected,true)){
            for(size_t i=0;i<NS6_CAPTURE_READERS;i++)if(atomic_load(&readers->slots[i].key)==key){slot=&readers->slots[i];break;}
            if(!slot)for(size_t i=0;i<NS6_CAPTURE_READERS;i++){
                uint64_t empty=0;
                if(atomic_compare_exchange_strong(&readers->slots[i].key,&empty,key)){slot=&readers->slots[i];break;}
            }
            atomic_store(&readers->registry_busy,false);
        }
    }
    bool expected=false;
    if(!slot || !atomic_compare_exchange_strong(&slot->busy,&expected,true)){
        for(size_t i=0;i<frames*2;i++)out[i]=0; return 0;
    }
    size_t result=0;
    if(atomic_load(&slot->key)==key && !atomic_load(&slot->retired)){
        if(slot->session!=session||slot->epoch!=epoch){ns6_capture_fanout_reader_init(&slot->reader,ring);slot->session=session;slot->epoch=epoch;atomic_store(&slot->dropped,0); }
        result=ns6_capture_fanout_read(ring,&slot->reader,out,frames);
        atomic_store_explicit(&slot->dropped,slot->reader.dropped,memory_order_relaxed);
    }else for(size_t i=0;i<frames*2;i++)out[i]=0;
    if(atomic_exchange(&slot->retired,false)){slot->session=slot->epoch=0;atomic_store(&slot->key,0);}
    atomic_store(&slot->busy,false);
    if(atomic_load(&slot->retired)){
        bool available=false;
        if(atomic_compare_exchange_strong(&slot->busy,&available,true)){
            if(atomic_exchange(&slot->retired,false)){slot->session=slot->epoch=0;atomic_store(&slot->key,0);}
            atomic_store(&slot->busy,false);
        }
    }
    return result;
}
static inline void ns6_capture_readers_remove(NS6CaptureReaders *readers,uint32_t client) {
    uint64_t key=(uint64_t)client+1;
    bool unlocked=false;
    while(!atomic_compare_exchange_weak(&readers->registry_busy,&unlocked,true)){unlocked=false;sched_yield();}
    for(size_t i=0;i<NS6_CAPTURE_READERS;i++){
        NS6CaptureReaderSlot *slot=&readers->slots[i];
        if(atomic_load(&slot->key)!=key)continue;
        atomic_store(&slot->retired,true);
        bool expected=false;
        if(atomic_compare_exchange_strong(&slot->busy,&expected,true)){
            if(atomic_exchange(&slot->retired,false)){slot->session=slot->epoch=0;atomic_store(&slot->key,0);}
            atomic_store(&slot->busy,false);
        }
        atomic_store(&readers->registry_busy,false);
        return;
    }
    atomic_store(&readers->registry_busy,false);
}
#endif
