#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include "solar_os_native_input.h"
#include "solar_os_native_input_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os.h"
#define solar_os_memory_calloc backend_memory_calloc
#define solar_os_memory_free backend_memory_free
#define main legacy_input_checks
#include "input_test.c"
#undef main
#undef solar_os_memory_calloc
#undef solar_os_memory_free

static void *callback_allocations[128];
static unsigned live_callbacks,retained,released,next_lease=100;
static bool fail_callback_allocation,leases[1024]={[42]=true};
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t kind,const char *tag)
{
    if(strcmp(tag,"native.input.callback"))return backend_memory_calloc(n,size,kind,tag);
    assert(n==1 && kind==SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    if(fail_callback_allocation)return NULL;
    void *p=calloc(n,size);assert(p);
    for(unsigned i=0;i<128;++i)if(!callback_allocations[i]){callback_allocations[i]=p;++live_callbacks;return p;}
    abort();
}
void solar_os_memory_free(void *p)
{
    for(unsigned i=0;i<128;++i)if(p && callback_allocations[i]==p){callback_allocations[i]=NULL;--live_callbacks;free(p);return;}
    backend_memory_free(p);
}
int solar_os_native_library_retain(uint32_t lease,uint32_t *out)
{
    *out=0;if(lease>=1024 || !leases[lease])return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *out=++next_lease;leases[*out]=true;++retained;return SOLAR_OS_NATIVE_OK;
}
int solar_os_native_library_release(uint32_t lease)
{
    if(lease>=1024 || !leases[lease])return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    leases[lease]=false;if(lease>100)++released;return SOLAR_OS_NATIVE_OK;
}
const solar_os_app_t *solar_os_application_descriptor(void) { return NULL; }
static const solar_os_native_input_api_v1_t *api;
static solar_os_input_source_t source;
static solar_os_native_handle_t observer_handle,filter_handle;
static atomic_bool entered,blocked,finish_callback;
static atomic_uint calls;
static bool close_self;
static bool publish_during_registration;
static void observe(const solar_os_input_gesture_event_t *event,void *user)
{
    assert(event && user==&calls && retained>released);atomic_fetch_add(&calls,1);
    if(close_self)assert(api->observer_close(observer_handle)==SOLAR_OS_NATIVE_ERROR_BUSY);
    atomic_store(&entered,true);
    while(atomic_load(&blocked) && !atomic_load(&finish_callback))sched_yield();
}
static bool filter(const solar_os_input_pointer_event_t *event,void *user)
{
    assert(event && user==&calls && retained>released);atomic_fetch_add(&calls,1);
    assert(api->filter_close(filter_handle)==SOLAR_OS_NATIVE_ERROR_BUSY);return true;
}
static void *publish(void *unused)
{
    (void)unused;const solar_os_input_gesture_event_t event={.gesture=SOLAR_OS_INPUT_GESTURE_FLICK};
    assert(!solar_os_input_write_gesture(source,&event));return NULL;
}
esp_err_t __real_solar_os_input_gesture_observer_register(solar_os_input_gesture_observer_t observer,void *user);
esp_err_t __wrap_solar_os_input_gesture_observer_register(solar_os_input_gesture_observer_t observer,void *user)
{
    esp_err_t result=__real_solar_os_input_gesture_observer_register(observer,user);
    if(!result && publish_during_registration)publish(NULL);
    return result;
}
static int dummy_destroy(void *value) { assert(value);return SOLAR_OS_NATIVE_OK; }
int main(void)
{
    assert(!solar_os_input_init());api=solar_os_native_input_get_service("input",1,sizeof(*api));assert(api);
    assert(!solar_os_input_source_open_typed("gesture",SOLAR_OS_INPUT_SOURCE_GESTURE,SOLAR_OS_INPUT_CAP_GESTURE_EVENTS,true,&source));
    solar_os_native_input_observer_v1_t descriptor={.struct_size=sizeof(descriptor),.library_lease=42,.observer=observe,.user=&calls};
    solar_os_native_handle_t token=999;
    descriptor.library_lease=0;
    assert(api->observer_open(&descriptor,&token)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !token && !live_callbacks);
    descriptor.library_lease=999;
    assert(api->observer_open(&descriptor,&token)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !token && !live_callbacks);
    descriptor.library_lease=42;fail_callback_allocation=true;
    assert(api->observer_open(&descriptor,&token)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !token && !live_callbacks);fail_callback_allocation=false;
    assert(!api->observer_open(&descriptor,&observer_handle) && live_callbacks==1);
    descriptor.observer=NULL; /* Registration consumed the descriptor. */
    atomic_store(&blocked,true);pthread_t worker;assert(!pthread_create(&worker,NULL,publish,NULL));
    while(!atomic_load(&entered))sched_yield();
    assert(api->observer_close(observer_handle)==SOLAR_OS_NATIVE_ERROR_BUSY && live_callbacks==1 && retained>released);
    publish(NULL);assert(atomic_load(&calls)==1); /* No new delivery after retirement. */
    atomic_store(&finish_callback,true);assert(!pthread_join(worker,NULL));
    assert(!api->observer_close(observer_handle) && !live_callbacks && retained==released);
    assert(api->observer_close(observer_handle)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    descriptor.observer=observe;close_self=true;atomic_store(&blocked,false);
    assert(!api->observer_open(&descriptor,&observer_handle));publish(NULL);
    unsigned before=atomic_load(&calls);publish(NULL);assert(atomic_load(&calls)==before);
    assert(!api->observer_close(observer_handle) && !live_callbacks);
    publish_during_registration=true;
    assert(!api->observer_open(&descriptor,&observer_handle));
    publish_during_registration=false;
    before=atomic_load(&calls);publish(NULL);assert(atomic_load(&calls)==before);
    assert(!api->observer_close(observer_handle) && !live_callbacks);
    solar_os_native_input_filter_v1_t filter_descriptor={.struct_size=sizeof(filter_descriptor),.library_lease=42,.filter=filter,.user=&calls};
    assert(!api->filter_open(&filter_descriptor,&filter_handle));
    assert(api->filter_open(&filter_descriptor,&token)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !token && live_callbacks==1);
    assert(api->observer_close(filter_handle)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    const solar_os_input_pointer_event_t point={.mode=SOLAR_OS_INPUT_POINTER_ABSOLUTE};
    assert(solar_os_input_pointer_filter_event(&point));before=atomic_load(&calls);
    assert(!solar_os_input_pointer_filter_event(&point) && atomic_load(&calls)==before);
    assert(!api->filter_close(filter_handle) && !live_callbacks && retained==released && !solar_os_native_handle_count());
    solar_os_native_handle_t observers[4];
    for(unsigned i=0;i<4;++i)assert(!api->observer_open(&descriptor,&observers[i]));
    assert(api->observer_open(&descriptor,&token)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !token && live_callbacks==4);
    for(unsigned i=0;i<4;++i)assert(!api->observer_close(observers[i]));
    solar_os_native_handle_t handles[128];
    for(unsigned i=0;i<128;++i)assert(!solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_EXPR,&calls,dummy_destroy,&handles[i]));
    assert(api->observer_open(&descriptor,&token)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !token && !live_callbacks && retained==released);
    for(unsigned i=0;i<128;++i)assert(!solar_os_native_handle_close_kind(handles[i],SOLAR_OS_NATIVE_HANDLE_EXPR));
    solar_os_input_source_close(source);
    return 0;
}
