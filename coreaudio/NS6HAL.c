#include "NS6Transport.h"
#include "NS6CaptureTimestamp.h"
#include "NS6CaptureReaders.h"
#include "NS6SharedMemory.h"
#include "NS6BufferFrameSize.h"
#include "NS6ConnectionPolicy.h"
#include "NS6ZeroTimestamp.h"
#include "NS6ActiveFlow.h"
#include "NS6IOClientAccounting.h"
#include "NS6IOMetrics.h"
#include "NS6DeviceClient.h"
#include "NS6HALMapping.h"
#include <CoreAudio/AudioHardware.h>
#include <CoreAudio/AudioServerPlugIn.h>
#include <CoreFoundation/CFPlugInCOM.h>
#include <IOKit/IOKitLib.h>
#include <IOKit/usb/IOUSBLib.h>
#include <mach/mach_time.h>
#include <math.h>
#include <os/log.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum { DEVICE_ID=2, STREAM_ID=3, INPUT_STREAM_ID=4, INPUT_DEVICE_ID=5 };
#define SAMPLE_RATE 44100.0
#define CHANNELS 4
/* Bounded diagnostic history: callbacks only update lock-free atomic fields. */
#define NS6_TRACE_CAPACITY 1024u
typedef struct { atomic_uint_fast64_t published, sample, time_ns, cycle; atomic_uint client, frames, valid; } NS6BlockTrace;
static NS6BlockTrace block_trace[NS6_TRACE_CAPACITY];
static atomic_uint_fast64_t block_trace_next;
static atomic_bool block_trace_dump;

static NS6BlockTrace input_block_trace[NS6_TRACE_CAPACITY];
static atomic_uint_fast64_t input_block_trace_next;
static atomic_bool input_block_trace_dump;

static void trace_output_block(UInt32 client, UInt32 frames, UInt64 sample, bool valid, UInt64 cycle, UInt64 time_ns){
    uint64_t n=atomic_fetch_add_explicit(&block_trace_next,1,memory_order_relaxed);
    NS6BlockTrace *t=&block_trace[n%NS6_TRACE_CAPACITY];
    atomic_store_explicit(&t->published,0,memory_order_release);
    atomic_store_explicit(&t->client,client,memory_order_relaxed);
    atomic_store_explicit(&t->frames,frames,memory_order_relaxed);
    atomic_store_explicit(&t->sample,sample,memory_order_relaxed);
    atomic_store_explicit(&t->valid,valid,memory_order_relaxed);
    atomic_store_explicit(&t->cycle,cycle,memory_order_relaxed);
    atomic_store_explicit(&t->time_ns,time_ns,memory_order_relaxed);
    atomic_store_explicit(&t->published,n+1,memory_order_release);
}
static void trace_dump_recent(void){
    if(!atomic_exchange_explicit(&block_trace_dump,false,memory_order_acq_rel))return;
    uint64_t end=atomic_load_explicit(&block_trace_next,memory_order_acquire);
    uint64_t begin=end>128?end-128:0;
    for(uint64_t n=begin;n<end;++n){
        NS6BlockTrace *t=&block_trace[n%NS6_TRACE_CAPACITY];
        if(atomic_load_explicit(&t->published,memory_order_acquire)!=n+1)continue;
        unsigned client=atomic_load_explicit(&t->client,memory_order_relaxed);
        unsigned frames=atomic_load_explicit(&t->frames,memory_order_relaxed);
        uint64_t sample=atomic_load_explicit(&t->sample,memory_order_relaxed);
        unsigned valid=atomic_load_explicit(&t->valid,memory_order_relaxed);
        uint64_t cycle=atomic_load_explicit(&t->cycle,memory_order_relaxed);
        uint64_t stamp=atomic_load_explicit(&t->time_ns,memory_order_relaxed);
        if(atomic_load_explicit(&t->published,memory_order_acquire)!=n+1)continue;
        os_log(OS_LOG_DEFAULT,"NS6 block trace: n=%llu client=%u frames=%u sample=%llu valid=%u cycle=%llu monotonic_ns=%llu",(unsigned long long)n,client,frames,(unsigned long long)sample,valid,(unsigned long long)cycle,(unsigned long long)stamp);
    }
}
static void trace_input_block(UInt32 client, UInt32 frames, UInt64 sample, bool valid, UInt64 cycle, UInt64 time_ns){
    uint64_t n=atomic_fetch_add_explicit(&input_block_trace_next,1,memory_order_relaxed);
    NS6BlockTrace *t=&input_block_trace[n%NS6_TRACE_CAPACITY];
    atomic_store_explicit(&t->published,0,memory_order_release);
    atomic_store_explicit(&t->client,client,memory_order_relaxed);
    atomic_store_explicit(&t->frames,frames,memory_order_relaxed);
    atomic_store_explicit(&t->sample,sample,memory_order_relaxed);
    atomic_store_explicit(&t->valid,valid,memory_order_relaxed);
    atomic_store_explicit(&t->cycle,cycle,memory_order_relaxed);
    atomic_store_explicit(&t->time_ns,time_ns,memory_order_relaxed);
    atomic_store_explicit(&t->published,n+1,memory_order_release);
}
static void trace_input_dump_recent(void){
    if(!atomic_exchange_explicit(&input_block_trace_dump,false,memory_order_acq_rel))return;
    uint64_t end=atomic_load_explicit(&input_block_trace_next,memory_order_acquire);
    uint64_t begin=end>16?end-16:0;
    for(uint64_t n=begin;n<end;++n){
        NS6BlockTrace *t=&input_block_trace[n%NS6_TRACE_CAPACITY];
        if(atomic_load_explicit(&t->published,memory_order_acquire)!=n+1)continue;
        unsigned client=atomic_load_explicit(&t->client,memory_order_relaxed);
        unsigned frames=atomic_load_explicit(&t->frames,memory_order_relaxed);
        uint64_t sample=atomic_load_explicit(&t->sample,memory_order_relaxed);
        unsigned valid=atomic_load_explicit(&t->valid,memory_order_relaxed);
        uint64_t cycle=atomic_load_explicit(&t->cycle,memory_order_relaxed);
        uint64_t stamp=atomic_load_explicit(&t->time_ns,memory_order_relaxed);
        if(atomic_load_explicit(&t->published,memory_order_acquire)!=n+1)continue;
        os_log(OS_LOG_DEFAULT,"NS6 input block trace: n=%llu client=%u frames=%u sample=%llu valid=%u cycle=%llu monotonic_ns=%llu",(unsigned long long)n,client,frames,(unsigned long long)sample,valid,(unsigned long long)cycle,(unsigned long long)stamp);
    }
}
#define NS6_DRIVER_BUFFER_PROPERTY ((AudioObjectPropertySelector)0x6e733662u) /* 'ns6b' */
#define NS6_BUFFER_RESTART_PROPERTY ((AudioObjectPropertySelector)0x6e733672u) /* 'ns6r' */
#define NS6_ACTIVE_IO_FRAMES_PROPERTY ((AudioObjectPropertySelector)0x6e733666u) /* 'ns6f' */
#define NS6_ACTIVE_CLIENT_PROPERTY ((AudioObjectPropertySelector)0x6e733663u) /* 'ns6c' */
#define NS6_FIRMWARE_VERSION_PROPERTY ((AudioObjectPropertySelector)0x6e733676u) /* 'ns6v' */
#define NS6_DRIVER_BUFFER_STORAGE_KEY CFSTR("driver-startup-buffer-frames")
#define NS6_TRACKED_CLIENTS 128 /* CoreAudio can register more than 32 clients. */
typedef struct { UInt32 client_id; AudioObjectID device; pid_t pid; char bundle_id[256]; unsigned starts; UInt64 order; bool used; } NS6TrackedClient;
static NS6TrackedClient tracked_clients[NS6_TRACKED_CLIENTS];
static UInt64 client_order;
static const UInt32 supported_buffer_frames[]={49,128,192,256,512,1024};
static const AudioServerPlugInCustomPropertyInfo custom_property_info[]={{NS6_DRIVER_BUFFER_PROPERTY,kAudioServerPlugInCustomPropertyDataTypeCFString,kAudioServerPlugInCustomPropertyDataTypeNone},{NS6_BUFFER_RESTART_PROPERTY,kAudioServerPlugInCustomPropertyDataTypeCFString,kAudioServerPlugInCustomPropertyDataTypeNone},{NS6_ACTIVE_IO_FRAMES_PROPERTY,kAudioServerPlugInCustomPropertyDataTypeCFString,kAudioServerPlugInCustomPropertyDataTypeNone},{NS6_ACTIVE_CLIENT_PROPERTY,kAudioServerPlugInCustomPropertyDataTypeCFString,kAudioServerPlugInCustomPropertyDataTypeNone},{NS6_FIRMWARE_VERSION_PROPERTY,kAudioServerPlugInCustomPropertyDataTypeCFString,kAudioServerPlugInCustomPropertyDataTypeNone}};
static atomic_uint buffer_frames=256;
static atomic_uint driver_buffer_frames=256;
static atomic_uint buffer_size_read_log_count;
static atomic_uint buffer_range_read_log_count;
static NS6ActiveFlow active_flow;
static atomic_bool buffer_restart_pending;
static atomic_uint buffer_restart_state;

static AudioServerPlugInHostRef host;
static UInt32 references=1,seed=1;
static atomic_uint io_clients;
static atomic_uint input_io_clients;
static unsigned untracked_io_starts;
static unsigned untracked_input_io_starts;
static UInt64 anchor_host_time;
static mach_timebase_info_data_t timebase;
static NS6CaptureReaders capture_readers;
static void initialize_io_metrics(void){ns6_io_metrics_init(&ns6_io_metrics);ns6_capture_readers_init(&capture_readers);}
static uint64_t io_monotonic_ns(void){struct timespec now;if(clock_gettime(CLOCK_MONOTONIC,&now)!=0)return 0;return (uint64_t)now.tv_sec*1000000000ull+(uint64_t)now.tv_nsec;}
static IONotificationPortRef usb_notifications;
static io_iterator_t usb_added,usb_removed;
static atomic_bool device_present;
static atomic_uint enqueue_failures;
static atomic_uint_fast64_t last_enqueue_log_ms;
static pthread_once_t usb_monitor_once=PTHREAD_ONCE_INIT;
static pthread_once_t io_metrics_once=PTHREAD_ONCE_INIT;
static pthread_mutex_t stream_lock=PTHREAD_MUTEX_INITIALIZER;
static NS6DeviceClient device_client={.service_port=MACH_PORT_NULL};
static NS6HALMapping callback_mapping;
static atomic_bool service_ready;
static atomic_bool service_superseded;
static AudioStreamBasicDescription stream_format={SAMPLE_RATE,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagsNativeEndian|kAudioFormatFlagIsPacked,16,1,16,CHANNELS,32,0};
static AudioStreamBasicDescription input_stream_format={SAMPLE_RATE,kAudioFormatLinearPCM,kAudioFormatFlagIsFloat|kAudioFormatFlagsNativeEndian|kAudioFormatFlagIsPacked,8,1,8,2,32,0};

static HRESULT query_interface(void*,REFIID,LPVOID*); static ULONG add_ref(void*); static ULONG release_ref(void*);
static OSStatus initialize(AudioServerPlugInDriverRef,AudioServerPlugInHostRef); static OSStatus create_device(AudioServerPlugInDriverRef,CFDictionaryRef,const AudioServerPlugInClientInfo*,AudioObjectID*); static OSStatus destroy_device(AudioServerPlugInDriverRef,AudioObjectID);
static OSStatus add_client(AudioServerPlugInDriverRef,AudioObjectID,const AudioServerPlugInClientInfo*); static OSStatus remove_client(AudioServerPlugInDriverRef,AudioObjectID,const AudioServerPlugInClientInfo*); static OSStatus perform_change(AudioServerPlugInDriverRef,AudioObjectID,UInt64,void*); static OSStatus abort_change(AudioServerPlugInDriverRef,AudioObjectID,UInt64,void*);
static Boolean has_property(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*); static OSStatus is_settable(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*,Boolean*); static OSStatus property_size(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*,UInt32,const void*,UInt32*); static OSStatus get_property(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*,UInt32,const void*,UInt32,UInt32*,void*); static OSStatus set_property(AudioServerPlugInDriverRef,AudioObjectID,pid_t,const AudioObjectPropertyAddress*,UInt32,const void*,UInt32,const void*);
static OSStatus start_io(AudioServerPlugInDriverRef,AudioObjectID,UInt32); static OSStatus stop_io(AudioServerPlugInDriverRef,AudioObjectID,UInt32); static OSStatus zero_timestamp(AudioServerPlugInDriverRef,AudioObjectID,UInt32,Float64*,UInt64*,UInt64*); static OSStatus will_do(AudioServerPlugInDriverRef,AudioObjectID,UInt32,UInt32,Boolean*,Boolean*); static OSStatus begin_io(AudioServerPlugInDriverRef,AudioObjectID,UInt32,UInt32,UInt32,const AudioServerPlugInIOCycleInfo*); static OSStatus do_io(AudioServerPlugInDriverRef,AudioObjectID,AudioObjectID,UInt32,UInt32,UInt32,const AudioServerPlugInIOCycleInfo*,void*,void*); static OSStatus end_io(AudioServerPlugInDriverRef,AudioObjectID,UInt32,UInt32,UInt32,const AudioServerPlugInIOCycleInfo*);

static AudioServerPlugInDriverInterface interface={NULL,query_interface,add_ref,release_ref,initialize,create_device,destroy_device,add_client,remove_client,perform_change,abort_change,has_property,is_settable,property_size,get_property,set_property,start_io,stop_io,zero_timestamp,will_do,begin_io,do_io,end_io};
static AudioServerPlugInDriverInterface *interface_pointer=&interface;
static AudioServerPlugInDriverRef driver=&interface_pointer;

static bool usb_property_u16(io_registry_entry_t entry,CFStringRef key,UInt16 *out){CFTypeRef value=IORegistryEntryCreateCFProperty(entry,key,kCFAllocatorDefault,0);bool ok=value&&CFGetTypeID(value)==CFNumberGetTypeID()&&CFNumberGetValue(value,kCFNumberSInt16Type,out);if(value)CFRelease(value);return ok;}
static bool ns6_connected(void){io_iterator_t iterator=IO_OBJECT_NULL;io_service_t service;if(IOServiceGetMatchingServices(kIOMainPortDefault,IOServiceMatching("IOUSBHostDevice"),&iterator)!=KERN_SUCCESS)return false;bool found=false;while((service=IOIteratorNext(iterator))){UInt16 vendor=0,product=0;if(usb_property_u16(service,CFSTR(kUSBVendorID),&vendor)&&usb_property_u16(service,CFSTR(kUSBProductID),&product)&&vendor==0x15e4&&product==0x0079)found=true;IOObjectRelease(service);if(found)break;}IOObjectRelease(iterator);return found;}
static void publish_connection_state(void){
    bool connected=ns6_connected();
    unsigned active_clients=atomic_load_explicit(&io_clients,memory_order_acquire);
    if(ns6_connection_should_defer_disconnect(connected,active_clients)){
        os_log(OS_LOG_DEFAULT,"Numark NS6 USB disconnected while %u CoreAudio clients are active; preserving the HAL device so clients can resume after USB recovery",active_clients);
        return;
    }
    if(!connected&&atomic_load_explicit(&device_present,memory_order_acquire)){
        /* USB enumeration can briefly lose the device while CoreAudio and the
           NS6 interfaces settle after boot. Don't withdraw the HAL device on
           one transient negative registry query. */
        const struct timespec settle={0,250000000};nanosleep(&settle,NULL);
        connected=ns6_connected();
        active_clients=atomic_load_explicit(&io_clients,memory_order_acquire);
        if(ns6_connection_should_defer_disconnect(connected,active_clients)){
            os_log(OS_LOG_DEFAULT,"Numark NS6 USB disconnected while %u CoreAudio clients are active; preserving the HAL device so clients can resume after USB recovery",active_clients);
            return;
        }
    }
    bool previous=atomic_exchange_explicit(&device_present,connected,memory_order_acq_rel);
    if(previous==connected)return;
    os_log(OS_LOG_DEFAULT,"Numark NS6 USB presence changed: %{public}s (confirmed)",connected?"connected":"disconnected");
    if(!host)return;
    AudioObjectPropertyAddress plugin_change={kAudioPlugInPropertyDeviceList,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    host->PropertiesChanged(host,kAudioObjectPlugInObject,1,&plugin_change);
    AudioObjectPropertyAddress device_change={kAudioDevicePropertyDeviceIsAlive,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};
    host->PropertiesChanged(host,DEVICE_ID,1,&device_change);
    host->PropertiesChanged(host,INPUT_DEVICE_ID,1,&device_change);
}
static void usb_changed(void *reference,io_iterator_t iterator){(void)reference;io_service_t service;while((service=IOIteratorNext(iterator)))IOObjectRelease(service);publish_connection_state();}
static void *usb_monitor_thread(void *unused){(void)unused;usb_notifications=IONotificationPortCreate(kIOMainPortDefault);if(!usb_notifications)return NULL;CFRunLoopAddSource(CFRunLoopGetCurrent(),IONotificationPortGetRunLoopSource(usb_notifications),kCFRunLoopDefaultMode);if(IOServiceAddMatchingNotification(usb_notifications,kIOFirstMatchNotification,IOServiceMatching("IOUSBHostDevice"),usb_changed,NULL,&usb_added)==KERN_SUCCESS)usb_changed(NULL,usb_added);if(IOServiceAddMatchingNotification(usb_notifications,kIOTerminatedNotification,IOServiceMatching("IOUSBHostDevice"),usb_changed,NULL,&usb_removed)==KERN_SUCCESS)usb_changed(NULL,usb_removed);CFRunLoopRun();return NULL;}
static void monitor_usb(void){pthread_t thread;if(pthread_create(&thread,NULL,usb_monitor_thread,NULL)==0)pthread_detach(thread);}

static void publish_device_mapping_locked(void){
    ns6_hal_mapping_withdraw(&callback_mapping);
    if(device_client.connected&&device_client.shared_memory){
        ns6_hal_mapping_publish(&callback_mapping,device_client.shared_memory,device_client.session_id);
        atomic_store_explicit(&service_ready,true,memory_order_release);
    }else atomic_store_explicit(&service_ready,false,memory_order_release);
}
static void log_io_metrics(void){
    NS6IOMetricsSnapshot snapshots[NS6_IO_METRICS_MAX_CLIENTS];
    size_t count=ns6_io_metrics_take_snapshot(&ns6_io_metrics,snapshots,NS6_IO_METRICS_MAX_CLIENTS);
    for(size_t n=0;n<count;++n){
        const NS6IOMetricsSnapshot *metric=&snapshots[n];
        if(!metric->calls)continue;
        double mean_frames=(double)metric->frames/(double)metric->calls;
        double mean_interval_us=metric->interval_count?
            (double)metric->interval_total_ns/(double)metric->interval_count/1000.0:0.0;
        os_log(OS_LOG_DEFAULT,"Numark NS6 CoreAudio output: client %u, %llu calls, mean %.1f frames/cycle, mean gap %.1f us, max gap %llu us, max callback %llu us, callback execution overruns %llu, duplicate cycles %llu, skipped cycles %llu, last cycle %llu, output sample time %{public}s:%llu, last frames %u",
               metric->client_id,(unsigned long long)metric->calls,mean_frames,mean_interval_us,
               (unsigned long long)(metric->max_interval_ns/1000),
               (unsigned long long)(metric->max_callback_ns/1000),
               (unsigned long long)metric->deadline_misses,
               (unsigned long long)metric->duplicate_cycles,
               (unsigned long long)metric->skipped_cycles,
               (unsigned long long)metric->last_cycle,
               metric->output_sample_time_valid?"valid":"invalid",
               (unsigned long long)metric->last_output_sample_time,
               metric->last_output_frames);
    }
}
static void *device_service_monitor(void *unused){
    (void)unused;
    const struct timespec interval={1,0};
    unsigned capture_log_tick=0;
    NS6CaptureTimestamp previous_capture={0,0};
    for(;;){
        nanosleep(&interval,NULL);
        pthread_once(&io_metrics_once,initialize_io_metrics);
        log_io_metrics();
        trace_dump_recent();
        if(capture_log_tick%5==0)atomic_store_explicit(&input_block_trace_dump,true,memory_order_release);
        trace_input_dump_recent();
        pthread_mutex_lock(&stream_lock);
        if(device_client.connected && device_client.shared_memory && ++capture_log_tick%5==0){
            for(size_t index=0;index<NS6_CAPTURE_READERS;index++){
                uint64_t key=atomic_load_explicit(&capture_readers.slots[index].key,memory_order_relaxed);
                uint64_t lost=atomic_load_explicit(&capture_readers.slots[index].dropped,memory_order_relaxed);
                if(key&&lost)os_log(OS_LOG_DEFAULT,"Numark NS6 capture reader %llu dropped %llu overwritten frames",(unsigned long long)(key-1),(unsigned long long)lost);
            }
            NS6CaptureRing *ring=&device_client.shared_memory->capture;
            NS6CaptureTimestamp current={0,0};
            uint64_t midpoint_ns=0;
            if(ns6_capture_ring_completion(ring,&current.frame,&current.host_ns) && current.host_ns!=previous_capture.host_ns){
                uint64_t midpoint=current.frame>=previous_capture.frame?previous_capture.frame+(current.frame-previous_capture.frame)/2:0;
                bool valid=previous_capture.host_ns && current.frame>=previous_capture.frame &&
                    ns6_capture_timestamp_interpolate(previous_capture,current,midpoint,&midpoint_ns);
                os_log(OS_LOG_DEFAULT,"Numark NS6 capture timing: produced frame %llu completion monotonic ns %llu, midpoint interpolation %{public}s %llu ns",
                    (unsigned long long)current.frame,(unsigned long long)current.host_ns,
                    valid?"valid":"unavailable",(unsigned long long)midpoint_ns);
                previous_capture=current;
            }
        }
        NS6DeviceStateSnapshot snapshot={0};
        NS6DeviceClientStateResult state_result=device_client.connected?
            ns6_device_client_get_state_result(&device_client,&snapshot):NS6_DEVICE_CLIENT_STATE_UNAVAILABLE;
        if(state_result==NS6_DEVICE_CLIENT_STATE_REPLACED){
            ns6_hal_mapping_withdraw(&callback_mapping);
            atomic_store_explicit(&service_ready,false,memory_order_release);
            if(!atomic_exchange_explicit(&service_superseded,true,memory_order_acq_rel))
                os_log(OS_LOG_DEFAULT,"Numark NS6 HAL session was superseded by a replacement helper; this helper will not reclaim USB ownership");
            pthread_mutex_unlock(&stream_lock);
            continue;
        }
        if(state_result==NS6_DEVICE_CLIENT_STATE_READY){
            atomic_store_explicit(&service_superseded,false,memory_order_release);
            if(!atomic_load_explicit(&service_ready,memory_order_acquire)){
                bool active=atomic_load_explicit(&io_clients,memory_order_acquire)>0;
                if(ns6_device_client_set_audio_active(&device_client,active)){
                    publish_device_mapping_locked();
                    os_log(OS_LOG_DEFAULT,"Numark NS6 HAL reattached to service session %llu after service readiness returned",device_client.session_id);
                }
            }
            pthread_mutex_unlock(&stream_lock);
            continue;
        }

        NS6DeviceClient replacement={.service_port=MACH_PORT_NULL};
        bool connected=ns6_device_client_connect(&replacement)&&
                       ns6_device_client_set_startup_buffer(&replacement,atomic_load_explicit(&driver_buffer_frames,memory_order_acquire));
        bool active=atomic_load_explicit(&io_clients,memory_order_acquire)>0;
        if(connected)connected=ns6_device_client_set_audio_active(&replacement,active);
        if(connected){
            NS6DeviceClient previous=device_client;
            device_client=replacement;
            publish_device_mapping_locked();
            atomic_store_explicit(&service_superseded,false,memory_order_release);
            ns6_device_client_disconnect(&previous);
            os_log(OS_LOG_DEFAULT,"Numark NS6 HAL reconnected to restarted USB service session %llu; active clients %u",device_client.session_id,atomic_load_explicit(&io_clients,memory_order_relaxed));
        }else{
            ns6_device_client_disconnect(&replacement);
            ns6_hal_mapping_withdraw(&callback_mapping);
            if(device_client.connected)ns6_device_client_disconnect(&device_client);
            atomic_store_explicit(&service_ready,false,memory_order_release);
            os_log_error(OS_LOG_DEFAULT,"Numark NS6 HAL service monitor could not reconnect; callbacks will remain silent and retry");
        }
        pthread_mutex_unlock(&stream_lock);
    }
    return NULL;
}
static void start_device_service_monitor(void){
    pthread_t thread;
    if(pthread_create(&thread,NULL,device_service_monitor,NULL)==0)pthread_detach(thread);
    else os_log_error(OS_LOG_DEFAULT,"Numark NS6 HAL failed to start its non-realtime service monitor");
}

void *NS6_Create(CFAllocatorRef allocator,CFUUIDRef type){(void)allocator;return CFEqual(type,kAudioServerPlugInTypeUUID)?driver:NULL;}
static bool valid_object(AudioObjectID object){return object==kAudioObjectPlugInObject||object==DEVICE_ID||object==INPUT_DEVICE_ID||object==STREAM_ID||object==INPUT_STREAM_ID;}
static bool supported_buffer_size(UInt32 frames);
static HRESULT query_interface(void *value,REFIID uuid,LPVOID *out){if(value!=driver||!out)return E_NOINTERFACE;CFUUIDRef requested=CFUUIDCreateFromUUIDBytes(NULL,uuid);HRESULT result=E_NOINTERFACE;if(requested&&(CFEqual(requested,IUnknownUUID)||CFEqual(requested,kAudioServerPlugInDriverInterfaceUUID))){*out=driver;++references;result=0;}if(requested)CFRelease(requested);return result;}
static ULONG add_ref(void *value){return value==driver?++references:0;} static ULONG release_ref(void *value){return value==driver&&references?--references:0;}
static OSStatus initialize(AudioServerPlugInDriverRef value,AudioServerPlugInHostRef value_host){
    if(value!=driver)return kAudioHardwareBadObjectError;
    host=value_host;
    pthread_once(&io_metrics_once,initialize_io_metrics);
    mach_timebase_info(&timebase);
    ns6_hal_mapping_init(&callback_mapping);
    device_client.service_port=MACH_PORT_NULL;
    if(host->CopyFromStorage){
        CFPropertyListRef stored=NULL;
        if(host->CopyFromStorage(host,NS6_DRIVER_BUFFER_STORAGE_KEY,&stored)==noErr&&stored&&CFGetTypeID(stored)==CFNumberGetTypeID()){
            SInt32 frames=0;
            if(CFNumberGetValue((CFNumberRef)stored,kCFNumberSInt32Type,&frames)&&frames>0&&supported_buffer_size((UInt32)frames))
                atomic_store(&driver_buffer_frames,(UInt32)frames);
        }
        if(stored)CFRelease(stored);
    }
    atomic_store(&device_present,ns6_connected());
    if(ns6_device_client_connect(&device_client)&&
       ns6_device_client_set_startup_buffer(&device_client,atomic_load(&driver_buffer_frames))){
        publish_device_mapping_locked();
        os_log(OS_LOG_DEFAULT,"Numark NS6 HAL mapped persistent device service session %llu",device_client.session_id);
    }else{
        ns6_device_client_disconnect(&device_client);
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 HAL could not connect to persistent USB service during initialize");
    }
    start_device_service_monitor();
    pthread_once(&usb_monitor_once,monitor_usb);
    return 0;
}
static OSStatus create_device(AudioServerPlugInDriverRef d,CFDictionaryRef x,const AudioServerPlugInClientInfo*y,AudioObjectID*z){(void)d;(void)x;(void)y;(void)z;return kAudioHardwareUnsupportedOperationError;} static OSStatus destroy_device(AudioServerPlugInDriverRef d,AudioObjectID x){(void)d;(void)x;return kAudioHardwareUnsupportedOperationError;}
static NS6TrackedClient *find_client_locked(AudioObjectID device,UInt32 client_id){for(size_t n=0;n<NS6_TRACKED_CLIENTS;++n)if(tracked_clients[n].used&&tracked_clients[n].client_id==client_id&&tracked_clients[n].device==device)return &tracked_clients[n];return NULL;}
static void refresh_client_identity_locked(void){NS6TrackedClient *latest=NULL;for(size_t n=0;n<NS6_TRACKED_CLIENTS;++n)if(tracked_clients[n].used&&tracked_clients[n].starts&&(!latest||tracked_clients[n].order>latest->order))latest=&tracked_clients[n];if(latest)os_log(OS_LOG_DEFAULT,"Numark NS6 active CoreAudio client: %{public}s pid=%d",latest->bundle_id,latest->pid);}
static OSStatus add_client(AudioServerPlugInDriverRef d,AudioObjectID object,const AudioServerPlugInClientInfo *info){(void)d;if((object!=DEVICE_ID&&object!=INPUT_DEVICE_ID)||!info)return 0;char bundle_id[256]="unknown";if(info->mBundleID)CFStringGetCString(info->mBundleID,bundle_id,sizeof(bundle_id),kCFStringEncodingUTF8);pthread_mutex_lock(&stream_lock);NS6TrackedClient *slot=find_client_locked(object,info->mClientID);bool existing=slot!=NULL;if(!slot)for(size_t n=0;n<NS6_TRACKED_CLIENTS;++n)if(!tracked_clients[n].used){slot=&tracked_clients[n];break;}if(slot){if(!existing)memset(slot,0,sizeof(*slot));slot->used=true;slot->client_id=info->mClientID;slot->device=object;slot->pid=info->mProcessID;strlcpy(slot->bundle_id,bundle_id,sizeof(slot->bundle_id));}pthread_mutex_unlock(&stream_lock);os_log(OS_LOG_DEFAULT,"Numark NS6 CoreAudio client added: id=%u pid=%d bundle=%{public}s tracked=%{public}s",info->mClientID,info->mProcessID,bundle_id,slot?"yes":"no");return 0;}
static OSStatus remove_client(AudioServerPlugInDriverRef d,AudioObjectID object,const AudioServerPlugInClientInfo *info){
    (void)d;
    if((object!=DEVICE_ID&&object!=INPUT_DEVICE_ID)||!info)return 0;
    if(object==INPUT_DEVICE_ID){pthread_once(&io_metrics_once,initialize_io_metrics);ns6_capture_readers_remove(&capture_readers,info->mClientID);}
    pid_t pid=0;
    char bundle_id[256]="unknown";
    unsigned released_starts=0;
    bool became_idle=false;
    pthread_mutex_lock(&stream_lock);
    unsigned clients=atomic_load_explicit(&io_clients,memory_order_acquire);
    NS6TrackedClient *slot=find_client_locked(object,info->mClientID);
    if(slot){
        pid=slot->pid;
        strlcpy(bundle_id,slot->bundle_id,sizeof(bundle_id));
        released_starts=ns6_io_client_account_remove(&slot->starts,&clients);
        memset(slot,0,sizeof(*slot));
        if(released_starts){
            if(object==INPUT_DEVICE_ID) atomic_fetch_sub_explicit(&input_io_clients,released_starts,memory_order_acq_rel);
            atomic_store_explicit(&io_clients,clients,memory_order_release);
            if(!clients){
                became_idle=true;
                ns6_active_flow_clear(&active_flow);
                (void)ns6_device_client_set_audio_active(&device_client,false);
                os_log(OS_LOG_DEFAULT,"Numark NS6 audio inactive after client removal; persistent service keeps the USB clock alive with silence (client %u)",info->mClientID);
            }
        }
    }
    refresh_client_identity_locked();
    pthread_mutex_unlock(&stream_lock);
    os_log(OS_LOG_DEFAULT,"Numark NS6 CoreAudio client removed: id=%u pid=%d bundle=%{public}s, released active starts %u, aggregate active clients %u",info->mClientID,pid,bundle_id,released_starts,atomic_load_explicit(&io_clients,memory_order_acquire));
    if(became_idle)publish_connection_state();
    return 0;
}
static bool supported_buffer_size(UInt32 frames){for(size_t i=0;i<sizeof(supported_buffer_frames)/sizeof(supported_buffer_frames[0]);++i)if(supported_buffer_frames[i]==frames)return true;return false;}
static OSStatus perform_change(AudioServerPlugInDriverRef d,AudioObjectID object,UInt64 action,void *info){(void)d;os_log(OS_LOG_DEFAULT,"Numark NS6 PerformDeviceConfigurationChange: object %u, action 0x%llx, info %{public}s, active clients %u, buffer %u frames",object,(unsigned long long)action,info?"present":"null",atomic_load_explicit(&io_clients,memory_order_acquire),atomic_load_explicit(&buffer_frames,memory_order_acquire));return (object==DEVICE_ID||object==INPUT_DEVICE_ID)?0:kAudioHardwareBadObjectError;}
static OSStatus abort_change(AudioServerPlugInDriverRef d,AudioObjectID object,UInt64 action,void*info){(void)d;os_log_error(OS_LOG_DEFAULT,"Numark NS6 AbortDeviceConfigurationChange: object %u, action 0x%llx, info %{public}s, active clients %u",object,(unsigned long long)action,info?"present":"null",atomic_load_explicit(&io_clients,memory_order_acquire));return (object==DEVICE_ID||object==INPUT_DEVICE_ID)?0:kAudioHardwareBadObjectError;}

static Boolean has_property(AudioServerPlugInDriverRef d,AudioObjectID object,pid_t pid,const AudioObjectPropertyAddress *address){
    (void)d;(void)pid;if(!valid_object(object)||!address)return false;
    switch(address->mSelector){case kAudioObjectPropertyBaseClass:case kAudioObjectPropertyClass:case kAudioObjectPropertyOwner:case kAudioObjectPropertyName:return true;default:break;}
    if(object==kAudioObjectPlugInObject)switch(address->mSelector){case kAudioObjectPropertyManufacturer:case kAudioObjectPropertyOwnedObjects:case kAudioPlugInPropertyDeviceList:case kAudioPlugInPropertyResourceBundle:return true;default:return false;}
    if(object==DEVICE_ID||object==INPUT_DEVICE_ID)switch(address->mSelector){case kAudioObjectPropertyManufacturer:case kAudioObjectPropertyOwnedObjects:case kAudioObjectPropertyCustomPropertyInfoList:case kAudioDevicePropertyDeviceUID:case kAudioDevicePropertyModelUID:case kAudioDevicePropertyTransportType:case kAudioDevicePropertyClockDomain:case kAudioDevicePropertyDeviceIsAlive:case kAudioDevicePropertyDeviceIsRunning:case kAudioDevicePropertyDeviceCanBeDefaultDevice:case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:case kAudioDevicePropertyLatency:case kAudioDevicePropertyStreams:case kAudioObjectPropertyControlList:case kAudioDevicePropertySafetyOffset:case kAudioDevicePropertyNominalSampleRate:case kAudioDevicePropertyAvailableNominalSampleRates:case kAudioDevicePropertyIsHidden:case kAudioDevicePropertyPreferredChannelsForStereo:case kAudioDevicePropertyBufferFrameSize:case kAudioDevicePropertyBufferFrameSizeRange:case kAudioDevicePropertyStreamConfiguration:case kAudioDevicePropertyZeroTimeStampPeriod:case NS6_DRIVER_BUFFER_PROPERTY:case NS6_BUFFER_RESTART_PROPERTY:case NS6_ACTIVE_IO_FRAMES_PROPERTY:case NS6_ACTIVE_CLIENT_PROPERTY:case NS6_FIRMWARE_VERSION_PROPERTY:return true;default:return false;}
    switch(address->mSelector){case kAudioStreamPropertyIsActive:case kAudioStreamPropertyDirection:case kAudioStreamPropertyTerminalType:case kAudioStreamPropertyStartingChannel:case kAudioStreamPropertyLatency:case kAudioStreamPropertyVirtualFormat:case kAudioStreamPropertyPhysicalFormat:case kAudioStreamPropertyAvailableVirtualFormats:case kAudioStreamPropertyAvailablePhysicalFormats:return true;default:return false;}
}
static OSStatus is_settable(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t p,const AudioObjectPropertyAddress*a,Boolean*out){if(!out)return kAudioHardwareIllegalOperationError;if(!has_property(d,o,p,a))return kAudioHardwareUnknownPropertyError;*out=o==DEVICE_ID&&(a->mSelector==NS6_DRIVER_BUFFER_PROPERTY||a->mSelector==kAudioDevicePropertyBufferFrameSize);return 0;}
static UInt32 stream_count(AudioObjectID object,const AudioObjectPropertyAddress *a){return ((object==DEVICE_ID&&a->mScope==kAudioObjectPropertyScopeInput)||(object==INPUT_DEVICE_ID&&a->mScope==kAudioObjectPropertyScopeOutput))?0:1;}
static const AudioStreamBasicDescription *format_for_stream(AudioObjectID stream){return stream==INPUT_STREAM_ID?&input_stream_format:&stream_format;}
static OSStatus property_size(AudioServerPlugInDriverRef d,AudioObjectID object,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 q,const void*x,UInt32 *out){
    (void)q;(void)x;if(!out)return kAudioHardwareIllegalOperationError;if(!has_property(d,object,pid,a))return kAudioHardwareUnknownPropertyError;
    switch(a->mSelector){
        case kAudioObjectPropertyName:case kAudioObjectPropertyManufacturer:case kAudioDevicePropertyDeviceUID:case kAudioDevicePropertyModelUID:case kAudioPlugInPropertyResourceBundle:*out=sizeof(CFStringRef);break;
        case kAudioObjectPropertyCustomPropertyInfoList:*out=object==DEVICE_ID?sizeof(custom_property_info):0;break;
        case NS6_DRIVER_BUFFER_PROPERTY:*out=sizeof(CFStringRef);break;
        case NS6_BUFFER_RESTART_PROPERTY:case NS6_ACTIVE_IO_FRAMES_PROPERTY:case NS6_ACTIVE_CLIENT_PROPERTY:case NS6_FIRMWARE_VERSION_PROPERTY:*out=sizeof(CFStringRef);break;
        case kAudioObjectPropertyOwnedObjects:*out=(object==kAudioObjectPlugInObject&&!atomic_load(&device_present))?0:(object==kAudioObjectPlugInObject?2*sizeof(AudioObjectID):((object==DEVICE_ID||object==INPUT_DEVICE_ID)?sizeof(AudioObjectID):0));break;
        case kAudioPlugInPropertyDeviceList:*out=atomic_load(&device_present)?2*sizeof(AudioObjectID):0;break;
        case kAudioDevicePropertyRelatedDevices:*out=sizeof(AudioObjectID);break;
        case kAudioDevicePropertyStreams:*out=stream_count(object,a)*sizeof(AudioObjectID);break;
        case kAudioObjectPropertyControlList:*out=0;break;
        case kAudioDevicePropertyNominalSampleRate:*out=sizeof(Float64);break;
        case kAudioDevicePropertyAvailableNominalSampleRates:case kAudioDevicePropertyBufferFrameSizeRange:*out=sizeof(AudioValueRange);break;
        case kAudioDevicePropertyPreferredChannelsForStereo:*out=2*sizeof(UInt32);break;
        case kAudioDevicePropertyStreamConfiguration:*out=offsetof(AudioBufferList,mBuffers)+stream_count(object,a)*sizeof(AudioBuffer);break;
        case kAudioStreamPropertyVirtualFormat:case kAudioStreamPropertyPhysicalFormat:*out=sizeof(AudioStreamBasicDescription);break;
        case kAudioStreamPropertyAvailableVirtualFormats:case kAudioStreamPropertyAvailablePhysicalFormats:*out=sizeof(AudioStreamRangedDescription);break;
        default:*out=sizeof(UInt32);break;
    }return 0;
}
static CFStringRef string_property(AudioObjectID object,AudioObjectPropertySelector selector){
    if(selector==kAudioObjectPropertyManufacturer)return CFSTR("Numark / community driver");
    if(selector==kAudioDevicePropertyDeviceUID)return object==INPUT_DEVICE_ID?CFSTR("io.github.maurojuniorr.numark-ns6.input"):CFSTR("io.github.maurojuniorr.numark-ns6.device");
    if(selector==kAudioDevicePropertyModelUID)return CFSTR("io.github.maurojuniorr.numark-ns6.original");
    if(selector==kAudioPlugInPropertyResourceBundle)return CFSTR("");
    if(object==kAudioObjectPlugInObject)return CFSTR("Numark NS6 Plug-In");
    if(object==INPUT_DEVICE_ID)return CFSTR("Numark NS6 Input"); if(object==DEVICE_ID)return CFSTR("Numark USB Audio Device"); if(object==INPUT_STREAM_ID)return CFSTR("Numark NS6 Input"); return CFSTR("Numark NS6 Output");
}
static OSStatus get_property(AudioServerPlugInDriverRef d,AudioObjectID object,pid_t pid,const AudioObjectPropertyAddress *a,UInt32 q,const void*x,UInt32 input_size,UInt32 *output_size,void *output){
    UInt32 needed=0;OSStatus status=property_size(d,object,pid,a,q,x,&needed);if(status)return status;if(input_size<needed)return kAudioHardwareBadPropertySizeError;if(output_size)*output_size=needed;if(!needed)return 0;if(!output)return kAudioHardwareIllegalOperationError;
    switch(a->mSelector){
        case kAudioObjectPropertyName:case kAudioObjectPropertyManufacturer:case kAudioDevicePropertyDeviceUID:case kAudioDevicePropertyModelUID:case kAudioPlugInPropertyResourceBundle:*(CFStringRef*)output=CFRetain(string_property(object,a->mSelector));return 0;
        case kAudioObjectPropertyCustomPropertyInfoList:if(object==DEVICE_ID)memcpy(output,custom_property_info,sizeof(custom_property_info));return 0;
        case NS6_DRIVER_BUFFER_PROPERTY:*(CFStringRef*)output=CFStringCreateWithFormat(kCFAllocatorDefault,NULL,CFSTR("%u"),atomic_load_explicit(&driver_buffer_frames,memory_order_acquire));return *(CFStringRef*)output?0:kAudioHardwareUnspecifiedError;
        case NS6_BUFFER_RESTART_PROPERTY:{UInt32 state=atomic_load_explicit(&buffer_restart_state,memory_order_acquire);const char *text=state==1?"restarting":state==2?"restarted":state==3?"failed":state==4?"applied":"idle";*(CFStringRef*)output=CFStringCreateWithCString(kCFAllocatorDefault,text,kCFStringEncodingUTF8);return *(CFStringRef*)output?0:kAudioHardwareUnspecifiedError;}
        case NS6_ACTIVE_IO_FRAMES_PROPERTY:{uint64_t max_age=2000000000ull*timebase.denom/timebase.numer;NS6ActiveFlowSnapshot flow=ns6_active_flow_snapshot(&active_flow,mach_absolute_time(),max_age);*(CFStringRef*)output=CFStringCreateWithFormat(kCFAllocatorDefault,NULL,CFSTR("%u"),flow.frames);return *(CFStringRef*)output?0:kAudioHardwareUnspecifiedError;}
        case NS6_FIRMWARE_VERSION_PROPERTY:{NS6DeviceStateSnapshot snapshot={0};bool available=ns6_device_client_get_state(&device_client,&snapshot)&&snapshot.connected;const char *value=available&&snapshot.firmware_version[0]?snapshot.firmware_version:"Unavailable";*(CFStringRef*)output=CFStringCreateWithCString(kCFAllocatorDefault,value,kCFStringEncodingUTF8);return *(CFStringRef*)output?0:kAudioHardwareUnspecifiedError;}
        case NS6_ACTIVE_CLIENT_PROPERTY:{pid_t active_pid=0;char bundle_id[256]={0};uint64_t max_age=2000000000ull*timebase.denom/timebase.numer;NS6ActiveFlowSnapshot flow=ns6_active_flow_snapshot(&active_flow,mach_absolute_time(),max_age);pthread_mutex_lock(&stream_lock);NS6TrackedClient *client=flow.frames?find_client_locked(DEVICE_ID,flow.client_id):NULL;if(client){active_pid=client->pid;memcpy(bundle_id,client->bundle_id,sizeof(bundle_id));}pthread_mutex_unlock(&stream_lock);*(CFStringRef*)output=CFStringCreateWithFormat(kCFAllocatorDefault,NULL,CFSTR("%d|%s"),active_pid,bundle_id);return *(CFStringRef*)output?0:kAudioHardwareUnspecifiedError;}
        case kAudioObjectPropertyBaseClass:*(AudioClassID*)output=kAudioObjectClassID;return 0;
        case kAudioObjectPropertyClass:*(AudioClassID*)output=object==kAudioObjectPlugInObject?kAudioPlugInClassID:((object==DEVICE_ID||object==INPUT_DEVICE_ID)?kAudioDeviceClassID:kAudioStreamClassID);return 0;
        case kAudioObjectPropertyOwner:*(AudioObjectID*)output=object==INPUT_STREAM_ID?INPUT_DEVICE_ID:(object==STREAM_ID?DEVICE_ID:kAudioObjectUnknown);return 0;
        case kAudioObjectPropertyOwnedObjects:if(object==kAudioObjectPlugInObject){((AudioObjectID*)output)[0]=DEVICE_ID;((AudioObjectID*)output)[1]=INPUT_DEVICE_ID;}else if(object==DEVICE_ID||object==INPUT_DEVICE_ID){*(AudioObjectID*)output=object==DEVICE_ID?STREAM_ID:INPUT_STREAM_ID;}return 0;
        case kAudioPlugInPropertyDeviceList:if(atomic_load(&device_present)){((AudioObjectID*)output)[0]=DEVICE_ID;((AudioObjectID*)output)[1]=INPUT_DEVICE_ID;}return 0;
        case kAudioDevicePropertyRelatedDevices:*(AudioObjectID*)output=object;return 0;
        case kAudioDevicePropertyStreams:*(AudioObjectID*)output=object==INPUT_DEVICE_ID?INPUT_STREAM_ID:STREAM_ID;return 0;
        case kAudioDevicePropertyNominalSampleRate:*(Float64*)output=SAMPLE_RATE;return 0;
        case kAudioDevicePropertyAvailableNominalSampleRates:{AudioValueRange r={SAMPLE_RATE,SAMPLE_RATE};*(AudioValueRange*)output=r;return 0;}
        case kAudioDevicePropertyBufferFrameSizeRange:{AudioValueRange r={supported_buffer_frames[0],supported_buffer_frames[sizeof(supported_buffer_frames)/sizeof(supported_buffer_frames[0])-1]};*(AudioValueRange*)output=r;unsigned log_count=atomic_fetch_add_explicit(&buffer_range_read_log_count,1,memory_order_relaxed);if(log_count<16)os_log(OS_LOG_DEFAULT,"Numark NS6 buffer range callback: pid %d scope 0x%x element %u => %.0f..%.0f frames",pid,a->mScope,a->mElement,r.mMinimum,r.mMaximum);return 0;}
        case kAudioDevicePropertyPreferredChannelsForStereo:((UInt32*)output)[0]=1;((UInt32*)output)[1]=2;return 0;
        case kAudioDevicePropertyStreamConfiguration:{AudioBufferList *list=output;list->mNumberBuffers=stream_count(object,a);if(list->mNumberBuffers){list->mBuffers[0].mNumberChannels=object==INPUT_DEVICE_ID?2:CHANNELS;list->mBuffers[0].mDataByteSize=0;list->mBuffers[0].mData=NULL;}return 0;}
        case kAudioStreamPropertyVirtualFormat:case kAudioStreamPropertyPhysicalFormat:*(AudioStreamBasicDescription*)output=*format_for_stream(object);return 0;
        case kAudioStreamPropertyAvailableVirtualFormats:case kAudioStreamPropertyAvailablePhysicalFormats:{const AudioStreamBasicDescription *format=format_for_stream(object);AudioStreamRangedDescription r={*format,{format->mSampleRate,format->mSampleRate}};*(AudioStreamRangedDescription*)output=r;return 0;}
        default:break;
    }
    UInt32 value=0;
    switch(a->mSelector){case kAudioDevicePropertyTransportType:value=kAudioDeviceTransportTypeUSB;break;case kAudioDevicePropertyDeviceIsAlive:value=atomic_load(&device_present)?1:0;break;case kAudioDevicePropertyDeviceCanBeDefaultDevice:case kAudioDevicePropertyDeviceCanBeDefaultSystemDevice:case kAudioStreamPropertyIsActive:value=1;break;case kAudioDevicePropertyDeviceIsRunning:{unsigned all=atomic_load(&io_clients),input=atomic_load(&input_io_clients);value=object==INPUT_DEVICE_ID?(input>0):(all>input);break;}case kAudioDevicePropertyBufferFrameSize:value=atomic_load_explicit(&buffer_frames,memory_order_acquire);break;case kAudioDevicePropertyZeroTimeStampPeriod:value=NS6_ZERO_TIMESTAMP_PERIOD;break;case kAudioStreamPropertyDirection:value=object==INPUT_STREAM_ID?1:0;break;case kAudioStreamPropertyTerminalType:value=kAudioStreamTerminalTypeLine;break;case kAudioStreamPropertyStartingChannel:value=1;break;default:value=0;break;}*(UInt32*)output=value;if(a->mSelector==kAudioDevicePropertyBufferFrameSize){unsigned log_count=atomic_fetch_add_explicit(&buffer_size_read_log_count,1,memory_order_relaxed);if(log_count<16)os_log(OS_LOG_DEFAULT,"Numark NS6 buffer size callback: pid %d scope 0x%x element %u => %u frames",pid,a->mScope,a->mElement,value);}return 0;
}
static OSStatus set_property(AudioServerPlugInDriverRef d,AudioObjectID o,pid_t p,const AudioObjectPropertyAddress*a,UInt32 q,const void*x,UInt32 n,const void*v){(void)d;(void)q;(void)x;if(o!=DEVICE_ID||!a)return kAudioHardwareUnsupportedOperationError;
    if(a->mSelector==kAudioDevicePropertyBufferFrameSize){
        if((a->mScope!=kAudioObjectPropertyScopeGlobal&&a->mScope!=kAudioObjectPropertyScopeOutput)||a->mElement!=kAudioObjectPropertyElementMain||n!=sizeof(UInt32)||!v){os_log(OS_LOG_DEFAULT,"Numark NS6 buffer size setter rejected malformed request: pid %d scope 0x%x element %u bytes %u",p,a->mScope,a->mElement,n);return kAudioHardwareBadPropertySizeError;}
        UInt32 frames=*(const UInt32*)v;
        if(!ns6_buffer_frame_size_request_valid(frames,supported_buffer_frames[0],supported_buffer_frames[sizeof(supported_buffer_frames)/sizeof(supported_buffer_frames[0])-1])){os_log(OS_LOG_DEFAULT,"Numark NS6 buffer size setter rejected value: pid %d scope 0x%x element %u requested %u allowed %u..%u",p,a->mScope,a->mElement,frames,supported_buffer_frames[0],supported_buffer_frames[sizeof(supported_buffer_frames)/sizeof(supported_buffer_frames[0])-1]);return kAudioHardwareIllegalOperationError;}
        UInt32 previous=atomic_exchange_explicit(&buffer_frames,frames,memory_order_acq_rel);
        os_log(OS_LOG_DEFAULT,"Numark NS6 CoreAudio buffer frame size set: requested %u, previous %u, client pid %d",frames,previous,p);
        if(host){AudioObjectPropertyAddress changed={kAudioDevicePropertyBufferFrameSize,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain};host->PropertiesChanged(host,o,1,&changed);}
        return noErr;
    }
    if(a->mSelector!=NS6_DRIVER_BUFFER_PROPERTY)return kAudioHardwareUnsupportedOperationError;
    if(a->mScope!=kAudioObjectPropertyScopeGlobal||a->mElement!=kAudioObjectPropertyElementMain||n!=sizeof(CFStringRef)||!v)return kAudioHardwareBadPropertySizeError;
    CFStringRef text=*(CFStringRef const*)v;
    if(!text)return kAudioHardwareIllegalOperationError;
    SInt32 value=CFStringGetIntValue(text);
    if(value<0||!supported_buffer_size((UInt32)value))return kAudioHardwareIllegalOperationError;
    UInt32 frames=(UInt32)value;
    if(atomic_load_explicit(&buffer_restart_pending,memory_order_acquire))return kAudioHardwareIllegalOperationError;
    if(!host||!host->WriteToStorage)return kAudioHardwareUnspecifiedError;
    CFNumberRef stored=CFNumberCreate(kCFAllocatorDefault,kCFNumberSInt32Type,&value);
    if(!stored)return kAudioHardwareUnspecifiedError;
    OSStatus status=host->WriteToStorage(host,NS6_DRIVER_BUFFER_STORAGE_KEY,stored);
    CFRelease(stored);
    if(status!=noErr)return status;
    atomic_store_explicit(&buffer_restart_pending,true,memory_order_release);
    atomic_store_explicit(&buffer_restart_state,1,memory_order_release);
    pthread_mutex_lock(&stream_lock);
    bool applied=atomic_load_explicit(&service_ready,memory_order_acquire)&&
                 ns6_device_client_set_startup_buffer(&device_client,frames);
    pthread_mutex_unlock(&stream_lock);
    if(applied){
        atomic_store_explicit(&driver_buffer_frames,frames,memory_order_release);
        atomic_store_explicit(&buffer_restart_state,4,memory_order_release);
        os_log(OS_LOG_DEFAULT,"Numark NS6 service accepted %u-frame startup buffer and queued the USB transport restart",frames);
    }else{
        atomic_store_explicit(&buffer_restart_state,3,memory_order_release);
        os_log_error(OS_LOG_DEFAULT,"Numark NS6 service rejected %u-frame startup buffer",frames);
    }
    atomic_store_explicit(&buffer_restart_pending,false,memory_order_release);
    AudioObjectPropertyAddress changed[2]={{NS6_DRIVER_BUFFER_PROPERTY,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain},{NS6_BUFFER_RESTART_PROPERTY,kAudioObjectPropertyScopeGlobal,kAudioObjectPropertyElementMain}};
    if(host)host->PropertiesChanged(host,o,2,changed);
    return applied?noErr:kAudioHardwareNotRunningError;
}

static OSStatus start_io(AudioServerPlugInDriverRef d,AudioObjectID object,UInt32 client){
    (void)d;
    if(object!=DEVICE_ID&&object!=INPUT_DEVICE_ID)return kAudioHardwareBadObjectError;
    pthread_once(&io_metrics_once,initialize_io_metrics);
    if(!atomic_load(&device_present)){os_log_error(OS_LOG_DEFAULT,"Numark NS6 start_io refused: USB device absent");return kAudioHardwareNotRunningError;}
    uint64_t start_io_begin_ns=io_monotonic_ns();
    pthread_mutex_lock(&stream_lock);
    uint64_t lock_acquired_ns=io_monotonic_ns();
    uint64_t lock_wait_us=lock_acquired_ns>=start_io_begin_ns?(lock_acquired_ns-start_io_begin_ns)/1000:0;
    unsigned clients=atomic_load_explicit(&io_clients,memory_order_acquire);
    unsigned clients_before=clients;
    bool service_available=atomic_load_explicit(&service_ready,memory_order_acquire)&&device_client.connected;
    bool activate_service=ns6_io_client_needs_service_activation(clients_before);
    bool activated=service_available;
    uint64_t activation_start_ns=0;
    if(activated&&activate_service){
        activation_start_ns=io_monotonic_ns();
        activated=ns6_device_client_set_audio_active(&device_client,true);
        uint64_t activation_end_ns=io_monotonic_ns();
        os_log(OS_LOG_DEFAULT,"Numark NS6 StartIO service activation: client %u, IPC %llu us, success %{public}s",
               client,(unsigned long long)(activation_end_ns>=activation_start_ns?(activation_end_ns-activation_start_ns)/1000:0),
               activated?"yes":"no");
    }
    bool reprimed_service=false;
    if(object==DEVICE_ID&&activated&&!activate_service&&device_client.shared_memory&&timebase.numer){
        uint64_t now=mach_absolute_time();
        uint64_t stale_after_ticks=(100000000ull*timebase.denom)/timebase.numer;
        NS6ActiveFlowSnapshot flow=ns6_active_flow_snapshot(&active_flow,now,stale_after_ticks);
        unsigned queued=ns6_transport_available_shared(&device_client.shared_memory->output);
        if(ns6_active_flow_should_rearm(flow,queued,1024)){
            uint64_t last=atomic_load_explicit(&active_flow.last_callback_tick,memory_order_acquire);
            uint64_t idle_ticks=last&&now>=last?now-last:UINT64_MAX;
            uint64_t idle_us=idle_ticks==UINT64_MAX?UINT64_MAX:
                (idle_ticks*timebase.numer)/(timebase.denom*1000ull);
            bool paused=ns6_device_client_set_audio_active(&device_client,false);
            activated=paused&&ns6_device_client_set_audio_active(&device_client,true);
            reprimed_service=activated;
            os_log(OS_LOG_DEFAULT,"Numark NS6 StartIO re-primed stale output: client %u, queued %u frames, last output callback %llu us ago, success %{public}s",
                   client,queued,(unsigned long long)idle_us,activated?"yes":"no");
        }
    }
    if(!activated){uint64_t start_io_end_ns=io_monotonic_ns();pthread_mutex_unlock(&stream_lock);os_log_error(OS_LOG_DEFAULT,"Numark NS6 StartIO failed: persistent USB service unavailable, lock wait %llu us, StartIO total %llu us",
        (unsigned long long)lock_wait_us,(unsigned long long)(start_io_end_ns>=start_io_begin_ns?(start_io_end_ns-start_io_begin_ns)/1000:0));return kAudioHardwareUnspecifiedError;}
    atomic_store_explicit(&block_trace_dump,true,memory_order_release);
    NS6TrackedClient *active_client=find_client_locked(object,client);
    if(active_client){ns6_io_client_account_start(&active_client->starts,&clients);active_client->order=++client_order;refresh_client_identity_locked();}
    else{ns6_io_client_account_untracked_start(object==INPUT_DEVICE_ID?&untracked_input_io_starts:&untracked_io_starts,&clients);os_log_error(OS_LOG_DEFAULT,"Numark NS6 StartIO client %u was not tracked by AddDeviceClient; retaining an aggregate stop token",client);}
    os_log(OS_LOG_DEFAULT,"Numark NS6 StartIO accounting: client %u pid %d bundle %{public}s, client starts %u, untracked starts %u, aggregate %u -> %u",
           client,active_client?active_client->pid:0,active_client?active_client->bundle_id:"unknown",
           active_client?active_client->starts:0,untracked_io_starts,clients_before,clients);
    if(clients_before==0){
        UInt64 previous_anchor=anchor_host_time;
        anchor_host_time=ns6_zero_timestamp_keep_anchor(anchor_host_time,mach_absolute_time());
        if(!previous_anchor)++seed;
        atomic_store(&enqueue_failures,0);atomic_store(&last_enqueue_log_ms,0);
    }
    atomic_store_explicit(&io_clients,clients,memory_order_release);
    if(object==INPUT_DEVICE_ID) atomic_fetch_add_explicit(&input_io_clients,1,memory_order_acq_rel);
    uint64_t start_io_end_ns=io_monotonic_ns();
    os_log(OS_LOG_DEFAULT,"Numark NS6 audio stream resumed (sample rate %.0f, client %u; %u prior clients; service activation %s; StartIO lock wait %llu us; StartIO total %llu us)",SAMPLE_RATE,client,clients_before,activate_service?"requested":reprimed_service?"re-primed":"already active",
        (unsigned long long)lock_wait_us,(unsigned long long)(start_io_end_ns>=start_io_begin_ns?(start_io_end_ns-start_io_begin_ns)/1000:0));
    pthread_mutex_unlock(&stream_lock);
    return 0;
}
static OSStatus stop_io(AudioServerPlugInDriverRef d,AudioObjectID object,UInt32 client){
    (void)d;
    if(object!=DEVICE_ID&&object!=INPUT_DEVICE_ID)return kAudioHardwareBadObjectError;
    pthread_mutex_lock(&stream_lock);
    unsigned clients=atomic_load_explicit(&io_clients,memory_order_acquire);
    unsigned clients_before=clients;
    bool became_idle=false;
    NS6TrackedClient *active_client=find_client_locked(object,client);
    bool matched_start=ns6_io_client_account_stop(active_client?&active_client->starts:NULL,object==INPUT_DEVICE_ID?&untracked_input_io_starts:&untracked_io_starts,&clients);
    if(matched_start){
        if(object==INPUT_DEVICE_ID) atomic_fetch_sub_explicit(&input_io_clients,1,memory_order_acq_rel);
        atomic_store_explicit(&io_clients,clients,memory_order_release);
        if(!clients){
            became_idle=true;
            ns6_active_flow_clear(&active_flow);
            (void)ns6_device_client_set_audio_active(&device_client,false);
            os_log(OS_LOG_DEFAULT,"Numark NS6 audio inactive; persistent service keeps the USB clock alive with silence (client %u)",client);
        }
    }else os_log_error(OS_LOG_DEFAULT,"Numark NS6 ignoring unmatched StopIO for client %u (tracked starts %u, untracked starts %u, active starts %u)",client,active_client?active_client->starts:0,untracked_io_starts,clients);
    os_log(OS_LOG_DEFAULT,"Numark NS6 StopIO accounting: client %u pid %d bundle %{public}s, matched %{public}s, client starts %u, untracked starts %u, aggregate %u -> %u",
           client,active_client?active_client->pid:0,active_client?active_client->bundle_id:"unknown",
           matched_start?"yes":"no",active_client?active_client->starts:0,
           untracked_io_starts,clients_before,clients);
    refresh_client_identity_locked();
    pthread_mutex_unlock(&stream_lock);
    if(became_idle)publish_connection_state();
    return 0;
}
static OSStatus zero_timestamp(AudioServerPlugInDriverRef d,AudioObjectID object,UInt32 client,Float64 *sample,UInt64 *host_time,UInt64 *out_seed){(void)d;(void)client;if((object!=DEVICE_ID&&object!=INPUT_DEVICE_ID)||!sample||!host_time||!out_seed)return kAudioHardwareIllegalOperationError;NS6ZeroTimestamp value=ns6_zero_timestamp_calculate(anchor_host_time,mach_absolute_time(),timebase.numer,timebase.denom,SAMPLE_RATE);*sample=(Float64)value.sample_time;*host_time=value.host_time;*out_seed=seed;return 0;}
static OSStatus will_do(AudioServerPlugInDriverRef d,AudioObjectID object,UInt32 client,UInt32 operation,Boolean *will,Boolean *in_place){(void)d;(void)object;if(!will||!in_place)return kAudioHardwareIllegalOperationError;*will=(object==DEVICE_ID&&operation==kAudioServerPlugInIOOperationWriteMix)||(object==INPUT_DEVICE_ID&&operation==kAudioServerPlugInIOOperationReadInput);*in_place=true;os_log(OS_LOG_DEFAULT,"Numark NS6 WillDoIOOperation: client %u, operation 0x%08x, will do %{public}s, in place yes",client,operation,*will?"yes":"no");return 0;}
static OSStatus begin_io(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 c,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo*i){(void)d;(void)i;if(o==DEVICE_ID&&op==kAudioServerPlugInIOOperationWriteMix&&frames)ns6_active_flow_record(&active_flow,frames,c,mach_absolute_time());return 0;}
static OSStatus do_io(AudioServerPlugInDriverRef d,AudioObjectID object,AudioObjectID stream,UInt32 client,UInt32 operation,UInt32 frames,const AudioServerPlugInIOCycleInfo*i,void *main_buffer,void *secondary){
    (void)d;(void)secondary;
    if(object!=DEVICE_ID&&object!=INPUT_DEVICE_ID)return kAudioHardwareBadObjectError;
    if(object==INPUT_DEVICE_ID&&stream==INPUT_STREAM_ID){
        if(operation!=kAudioServerPlugInIOOperationReadInput)return kAudioHardwareIllegalOperationError;
        if(!main_buffer&&frames)return kAudioHardwareIllegalOperationError;
        NS6HALMappingLease lease;
        if(!ns6_hal_mapping_acquire(&callback_mapping,&lease)){
            if(main_buffer&&frames)memset(main_buffer,0,(size_t)frames*2*sizeof(float));
            return 0;
        }
        NS6SharedMemory *memory=lease.memory;
        if(!ns6_device_session_io_begin(&memory->session,lease.session_id)){
            if(main_buffer&&frames)memset(main_buffer,0,(size_t)frames*2*sizeof(float));
            ns6_hal_mapping_release(&callback_mapping);
            return 0;
        }
        uint64_t now_ns=io_monotonic_ns();
        bool valid_input_time=i&&(i->mInputTime.mFlags&kAudioTimeStampSampleTimeValid)&&
            isfinite(i->mInputTime.mSampleTime)&&i->mInputTime.mSampleTime>=0.0&&
            i->mInputTime.mSampleTime<(double)UINT64_MAX;
        trace_input_block(client,frames,valid_input_time?(uint64_t)i->mInputTime.mSampleTime:0,
            valid_input_time,i?i->mIOCycleCounter:0,now_ns);
        if(!atomic_exchange_explicit(&memory->capture_requested,true,memory_order_acq_rel)){
            ns6_capture_ring_clear(&memory->capture);
            atomic_fetch_add_explicit(&memory->capture_epoch,1,memory_order_acq_rel);
        }
        atomic_store_explicit(&memory->capture_last_demand_ns,now_ns,memory_order_release);
        (void)ns6_capture_readers_read(&capture_readers,&memory->capture_fanout,client,
            lease.session_id,atomic_load_explicit(&memory->capture_epoch,memory_order_acquire),
            (float*)main_buffer,frames);
        ns6_device_session_io_end(&memory->session);
        ns6_hal_mapping_release(&callback_mapping);
        return 0;
    }
    if(object!=DEVICE_ID||stream!=STREAM_ID)return kAudioHardwareBadObjectError;
    if(operation==kAudioServerPlugInIOOperationWriteMix){
        NS6HALMappingLease lease;
        if(!ns6_hal_mapping_acquire(&callback_mapping,&lease))return 0;
        NS6SharedMemory *memory=lease.memory;
        if(!ns6_device_session_io_begin(&memory->session,lease.session_id)){
            ns6_hal_mapping_release(&callback_mapping);
            return 0;
        }
        ns6_active_flow_record(&active_flow,frames,client,mach_absolute_time());
        uint64_t callback_start_ns=io_monotonic_ns();
        NS6TransportShared *output_ring=&memory->output;
        uint64_t output_sample_time=0;
        bool output_time_valid=false;
        if(i&&(i->mOutputTime.mFlags&kAudioTimeStampSampleTimeValid)&&
           isfinite(i->mOutputTime.mSampleTime)&&i->mOutputTime.mSampleTime>=0.0&&
           i->mOutputTime.mSampleTime<(double)UINT64_MAX){
            output_sample_time=(uint64_t)i->mOutputTime.mSampleTime;
            output_time_valid=true;
        }
        trace_output_block(client,frames,output_sample_time,output_time_valid,i?i->mIOCycleCounter:0,callback_start_ns);
        bool accepted=ns6_transport_enqueue_shared(output_ring,main_buffer,frames,&stream_format);
        uint64_t callback_end_ns=io_monotonic_ns();
        uint64_t callback_ns=callback_end_ns>=callback_start_ns?callback_end_ns-callback_start_ns:0;
        uint64_t deadline_ns=frames?(uint64_t)frames*1000000000ull/(uint64_t)SAMPLE_RATE:0;
        ns6_io_metrics_record_output_timed(&ns6_io_metrics,client,frames,
            i?i->mIOCycleCounter:0,output_sample_time,output_time_valid,
            callback_start_ns,callback_ns,deadline_ns);
        if(!accepted){
            unsigned failures=atomic_fetch_add(&enqueue_failures,1)+1;
            uint64_t now=mach_absolute_time();double now_ms=(double)now*timebase.numer/timebase.denom/1e6;
            uint_fast64_t previous=atomic_load_explicit(&last_enqueue_log_ms,memory_order_relaxed);
            if(failures==1||(now_ms-previous>=1000&&atomic_compare_exchange_strong_explicit(&last_enqueue_log_ms,&previous,(uint_fast64_t)now_ms,memory_order_relaxed,memory_order_relaxed)))
                os_log_error(OS_LOG_DEFAULT,"Numark NS6 could not enqueue CoreAudio output buffer: %u cumulative failures, client %u, sample time %{public}s:%llu, %u input frames, %u queued USB frames",failures,client,output_time_valid?"valid":"implicit",(unsigned long long)output_sample_time,frames,ns6_transport_available_shared(output_ring));
            ns6_device_session_io_end(&memory->session);
            ns6_hal_mapping_release(&callback_mapping);
            /* A dropped block must not tear down every CoreAudio client. */
            return 0;
        }
        ns6_device_session_io_end(&memory->session);
        ns6_hal_mapping_release(&callback_mapping);
    }
    return 0;
}
static OSStatus end_io(AudioServerPlugInDriverRef d,AudioObjectID o,UInt32 c,UInt32 op,UInt32 frames,const AudioServerPlugInIOCycleInfo*i){(void)d;(void)o;(void)c;(void)op;(void)frames;(void)i;return 0;}
