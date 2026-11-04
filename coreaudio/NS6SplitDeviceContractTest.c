#include "NS6HAL.c"
#include <assert.h>

int main(void) {
    AudioObjectPropertyAddress list = {kAudioPlugInPropertyDeviceList, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    atomic_store(&device_present, true);
    assert(property_size(driver, kAudioObjectPlugInObject, 0, &list, 0, NULL, &size) == noErr);
    assert(size == 2 * sizeof(AudioObjectID));
    AudioObjectID devices[2] = {0};
    assert(get_property(driver, kAudioObjectPlugInObject, 0, &list, 0, NULL, sizeof(devices), &size, devices) == noErr);
    assert(devices[0] != devices[1]);
    AudioObjectPropertyAddress owner = {kAudioObjectPropertyOwner, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectID input_owner = 0;
    assert(get_property(driver, INPUT_STREAM_ID, 0, &owner, 0, NULL, sizeof(input_owner), &size, &input_owner) == noErr);
    assert(input_owner == devices[1]);
    AudioObjectPropertyAddress streams = {kAudioDevicePropertyStreams, kAudioObjectPropertyScopeInput, kAudioObjectPropertyElementMain};
    assert(property_size(driver, devices[0], 0, &streams, 0, NULL, &size) == noErr && size == 0);
    assert(property_size(driver, devices[1], 0, &streams, 0, NULL, &size) == noErr && size == sizeof(AudioObjectID));
    streams.mScope = kAudioObjectPropertyScopeOutput;
    assert(property_size(driver, devices[1], 0, &streams, 0, NULL, &size) == noErr && size == 0);
    assert(property_size(driver, devices[0], 0, &streams, 0, NULL, &size) == noErr && size == sizeof(AudioObjectID));
    AudioServerPlugInClientInfo info = {.mClientID=42, .mProcessID=123};
    assert(add_client(driver, devices[0], &info) == noErr);
    assert(add_client(driver, devices[1], &info) == noErr);
    assert(find_client_locked(devices[0], 42) != find_client_locked(devices[1], 42));
    assert(remove_client(driver, devices[0], &info) == noErr);
    assert(find_client_locked(devices[0], 42) == NULL && find_client_locked(devices[1], 42) != NULL);
    assert(remove_client(driver, devices[1], &info) == noErr);
    Boolean input_read = false, output_read = true, in_place = false;
    assert(will_do(driver, devices[1], 0, kAudioServerPlugInIOOperationReadInput, &input_read, &in_place) == noErr);
    assert(will_do(driver, devices[0], 0, kAudioServerPlugInIOOperationReadInput, &output_read, &in_place) == noErr);
    assert(input_read && !output_read);
    static NS6SharedMemory memory;
    ns6_shared_memory_init(&memory);
    assert(ns6_device_session_begin(&memory.session,99));
    ns6_hal_mapping_init(&callback_mapping);
    ns6_hal_mapping_publish(&callback_mapping,&memory,99);
    pthread_once(&io_metrics_once,initialize_io_metrics);
    atomic_store(&memory.capture_requested,true);
    float a[4],b[4]; static float data[NS6_CAPTURE_RESERVE*2];
    for(unsigned j=0;j<NS6_CAPTURE_RESERVE*2;j++)data[j]=0.25f;
    AudioServerPlugInIOCycleInfo cycle={0};
    cycle.mInputTime.mFlags=kAudioTimeStampSampleTimeValid;
    cycle.mInputTime.mSampleTime=100;
    assert(do_io(driver,INPUT_DEVICE_ID,INPUT_STREAM_ID,7,kAudioServerPlugInIOOperationReadInput,2,&cycle,a,NULL)==noErr);
    ns6_capture_fanout_write(&memory.capture_fanout,data,NS6_CAPTURE_RESERVE);
    cycle.mInputTime.mSampleTime=102;
    assert(do_io(driver,INPUT_DEVICE_ID,INPUT_STREAM_ID,7,kAudioServerPlugInIOOperationReadInput,2,&cycle,a,NULL)==noErr);
    assert(do_io(driver,INPUT_DEVICE_ID,INPUT_STREAM_ID,8,kAudioServerPlugInIOOperationReadInput,2,&cycle,b,NULL)==noErr);
    for(int j=0;j<4;j++)assert(a[j]==0.25f&&b[j]==0.25f);
    ns6_hal_mapping_withdraw(&callback_mapping);
    return 0;
}
