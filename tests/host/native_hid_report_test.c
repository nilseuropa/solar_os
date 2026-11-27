#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "solar_os.h"
#include "solar_os_memory.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_hid_report.h"
#include "solar_os_native_hid_report_abi.h"
#include "solar_os_hid_keyboard_report.h"
#include "solar_os_hid_report_descriptor.h"
static const solar_os_native_hid_report_api_v1_t *api;
static bool fail_allocation, owned;
static unsigned allocations, owner_token;
static const solar_os_app_t owner_descriptor;
const solar_os_app_t *solar_os_application_descriptor(void)
{ return owned ? &owner_descriptor : NULL; }
void *solar_os_memory_calloc(size_t count,size_t size,solar_os_memory_class_t kind,const char *tag)
{
    assert(count==1 && size==sizeof(solar_os_hid_keyboard_report_tracker_t));
    assert(kind==SOLAR_OS_MEMORY_TRANSIENT && !strcmp(tag,"native.hid.report"));
    if(fail_allocation)return NULL;
    void *state=calloc(count,size);assert(state);++allocations;return state;
}
void solar_os_memory_free(void *state)
{ if(state){assert(allocations);--allocations;free(state);} }

/* Replay the existing behavioral corpus using opaque handles. This test-only
 * variable holds a handle instead of the kernel's private tracker structure. */
typedef struct { solar_os_native_handle_t handle; } replay_tracker_t;
static solar_os_native_handle_t replay_handles[16];
static size_t replay_count;
static solar_os_native_handle_t replay_handle(replay_tracker_t *tracker)
{
    if(!tracker->handle) {
        assert(replay_count<sizeof(replay_handles)/sizeof(replay_handles[0]));
        assert(!api->create(&tracker->handle));replay_handles[replay_count++]=tracker->handle;
    }
    return tracker->handle;
}
static void replay_reset(replay_tracker_t *tracker)
{ assert(!api->reset(replay_handle(tracker))); }
static bool replay_update(replay_tracker_t *tracker,uint8_t map_index,uint16_t report_id,
    const uint8_t *data,size_t length,solar_os_hid_keyboard_report_state_t *out)
{ return api->update(replay_handle(tracker),map_index,report_id,data,length,out); }
static bool replay_update_with_layout(replay_tracker_t *tracker,uint8_t map_index,uint16_t report_id,
    const uint8_t *data,size_t length,const solar_os_hid_keyboard_report_layout_t *layout,
    solar_os_hid_keyboard_report_state_t *out)
{ return api->update_with_layout(replay_handle(tracker),map_index,report_id,data,length,layout,out); }
#define solar_os_hid_keyboard_report_tracker_t replay_tracker_t
#define solar_os_hid_keyboard_report_reset replay_reset
#define solar_os_hid_keyboard_report_update replay_update
#define solar_os_hid_keyboard_report_update_with_layout replay_update_with_layout
#define solar_os_hid_keyboard_report_has_new_press api->has_new_press
#define solar_os_hid_report_descriptor_keyboard_map api->descriptor_map
#define solar_os_hid_report_descriptor_keyboard_layout api->descriptor_layout
#define main report_kernel_checks
#include "hid_keyboard_report_test.c"
#undef main
#undef solar_os_hid_keyboard_report_tracker_t
#undef solar_os_hid_keyboard_report_reset
#undef solar_os_hid_keyboard_report_update
#undef solar_os_hid_keyboard_report_update_with_layout
#undef solar_os_hid_keyboard_report_has_new_press
#undef solar_os_hid_report_descriptor_keyboard_map
#undef solar_os_hid_report_descriptor_keyboard_layout
static int no_destroy(void *state) { assert(state==&owner_token);return SOLAR_OS_NATIVE_OK; }
int main(void)
{
    api=solar_os_native_hid_report_get_service("hid.report",1,sizeof(*api));
    assert(api && api->abi_version==1 && api->struct_size==sizeof(*api));
    assert(!solar_os_native_hid_report_get_service("hid.report",2,8));
    assert(!solar_os_native_hid_report_get_service("hid.report",1,sizeof(*api)+1));
    assert(!solar_os_native_hid_report_get_service("hid",1,8));
    solar_os_native_hid_report_limits_v1_t limits;
    assert(api->limits(NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(!api->limits(&limits) && limits.report_streams && limits.report_keys==SOLAR_OS_HID_KEYBOARD_REPORT_KEYS);
    assert(limits.descriptor_report_bytes==SOLAR_OS_HID_DESCRIPTOR_REPORT_BYTES_MAX);
    assert(api->create(NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    solar_os_native_handle_t tracker=999;
    fail_allocation=true;
    assert(api->create(&tracker)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !tracker && !allocations);
    fail_allocation=false;
    assert(!report_kernel_checks());
    assert(allocations==replay_count);
    for(size_t i=0;i<replay_count;++i)assert(!api->close(replay_handles[i]));
    assert(!allocations && !solar_os_native_handle_count());

    owned=true;assert(!api->create(&tracker));owned=false;
    assert(solar_os_native_handle_acquire(tracker,SOLAR_OS_NATIVE_HANDLE_HID_REPORT));
    assert(api->close(tracker)==SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(solar_os_native_handle_close_owner(&owner_descriptor)==SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(!api->reset(tracker));
    solar_os_native_handle_release(tracker);
    assert(!solar_os_native_handle_close_owner(&owner_descriptor) && !allocations);
    assert(api->close(tracker)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(api->reset(tracker)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    solar_os_hid_keyboard_report_state_t snapshot={.modifiers=17,.keys={4}};
    const solar_os_hid_keyboard_report_state_t previous=snapshot;
    const uint8_t body[8]={0};
    assert(!api->update(tracker,0,0,body,sizeof(body),&snapshot) && !memcmp(&snapshot,&previous,sizeof(snapshot)));
    assert(!solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_SHA256,&owner_token,no_destroy,&tracker));
    assert(api->close(tracker)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && api->reset(tracker)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!solar_os_native_handle_close(tracker));

    solar_os_native_handle_t handles[256];size_t count=0;
    for(;count<sizeof(handles)/sizeof(handles[0]);++count) {
        int result=api->create(&handles[count]);
        if(result) { assert(result==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !handles[count]);break; }
    }
    assert(count && count<sizeof(handles)/sizeof(handles[0]) && allocations==count);
    for(size_t i=0;i<count;++i)assert(!api->close(handles[i]));
    assert(!allocations && !solar_os_native_handle_count());
    puts("HID report ABI: real decoding, opaque typed trackers, OOM/pool rollback, busy close and owned cleanup passed");
    return 0;
}
