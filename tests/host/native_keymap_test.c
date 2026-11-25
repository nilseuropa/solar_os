#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include "solar_os_native_keymap.h"
#include "solar_os_native_keymap_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#define main baseline_keymap_checks
#include "input_keymap_service_test.c"
#undef main

static unsigned live_adapters,retains,releases,next_lease=100;
static bool leases[1024]={[42]=true};
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t kind,const char *tag)
{
    assert(n==1 && kind==SOLAR_OS_MEMORY_EXTERNAL_PREFERRED && !strcmp(tag,"native.input.keymap"));
    if(++allocations==fail_allocation)return NULL;
    void *p=calloc(n,size);assert(p);++live_adapters;return p;
}
void solar_os_memory_free(void *p) { if(p){assert(live_adapters);--live_adapters;free(p);} }
int solar_os_native_library_retain(uint32_t lease,uint32_t *out)
{
    *out=0;if(lease>=1024 || !leases[lease])return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    *out=++next_lease;leases[*out]=true;++retains;return SOLAR_OS_NATIVE_OK;
}
int solar_os_native_library_release(uint32_t lease)
{
    if(lease>=1024 || !leases[lease])return SOLAR_OS_NATIVE_ERROR_NOT_FOUND;
    leases[lease]=false;++releases;return SOLAR_OS_NATIVE_OK;
}
static atomic_bool pause_lock,entered,continue_lock;
static unsigned unlocked;
static int native_lock(void *value)
{
    int result=solar_os_native_result(lock(value));
    if(!result && atomic_load(&pause_lock)){
        atomic_store(&entered,true);
        while(!atomic_load(&continue_lock))sched_yield();
    }
    return result;
}
static void native_unlock(void *value) { unlock(value);++unlocked; }
static int native_prepare(void *value) { return solar_os_native_result(prepare(value)); }
static const solar_os_native_keymap_api_v1_t *api;
static solar_os_input_keymap_t current;
static void *read_profile(void *unused)
{
    (void)unused;
    assert(api->get("source1",false,&current)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    return NULL;
}
static unsigned closed_sources;
static int producer_destroy(void *value) { assert(value);++closed_sources;return SOLAR_OS_NATIVE_OK; }
int main(void)
{
    assert(!baseline_keymap_checks());
    memset(&owner,0,sizeof(owner));event_count=0;read_error=ESP_OK;fail_allocation=0;
    api=solar_os_native_keymap_get_service("input.keymap",1,8);assert(api && api->struct_size==sizeof(*api));
    assert(!solar_os_native_keymap_get_service("input.keymap",2,8));
    assert(!solar_os_native_keymap_get_service("input.keymap",1,sizeof(*api)+1));
    assert(!solar_os_native_keymap_get_service("missing",1,8));
    assert(api->decode==solar_os_input_keymap_decode && api->release==solar_os_input_keymap_release);
    solar_os_native_handle_t producer,profile=999,other;
    assert(!solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER,(void *)(uintptr_t)1,producer_destroy,&producer));
    solar_os_input_keymap_t defaults={.slot_count=3};
    defaults.physical[1]=50000;defaults.keys[0][1].usage=4;
    defaults.physical[2]=900;defaults.keys[0][2].usage=0xe1;
    defaults.physical[3]=60000;defaults.keys[0][3]=(solar_os_input_keymap_key_t){.usage=0x2c,.flags=SOLAR_OS_INPUT_KEYMAP_LAYER_TAP,.layer=1};
    defaults.keys[1][1].key='!';
    assert(!api->validate(&defaults) && api->find_slot(&defaults,50000)==1);
    solar_os_native_keymap_provider_v1_t descriptor={.struct_size=sizeof(descriptor),.library_lease=42,
        .producer=producer,.defaults=&defaults,.lock=native_lock,.unlock=native_unlock,.prepare=native_prepare,.user=&owner};
    descriptor.library_lease=0;
    assert(api->provider_open(&descriptor,&profile)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !profile && !live_adapters);
    descriptor.library_lease=999;
    assert(api->provider_open(&descriptor,&profile)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !profile && !live_adapters);
    descriptor.library_lease=42;descriptor.producer=999;
    assert(api->provider_open(&descriptor,&profile)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !profile && !live_adapters);
    descriptor.producer=producer;
    fail_allocation=allocations+1;
    assert(api->provider_open(&descriptor,&profile)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !profile && !live_adapters);
    fail_allocation=allocations+2; /* Core profile allocation, after adapter allocation. */
    assert(api->provider_open(&descriptor,&profile)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !profile && !live_adapters && retains==releases);
    fail_allocation=0;
    assert(!api->provider_open(&descriptor,&profile) && live_adapters==1 && retains==releases+1);
    assert(solar_os_native_handle_close_kind(producer,SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER)==SOLAR_OS_NATIVE_ERROR_BUSY);
    assert(api->get("source1",false,&current)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->provider_open(&descriptor,&other)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !other && live_adapters==1);
    assert(!api->provider_activate(profile));
    solar_os_input_keymap_info_t info;
    assert(!api->info("source1",&info) && info.key_count==3 && (info.capabilities & SOLAR_OS_INPUT_KEYMAP_TAP_HOLD));
    defaults.keys[0][1].usage=5;descriptor.lock=NULL; /* Registry owns copies. */
    assert(!api->get("source1",true,&current) && current.keys[0][1].usage==4);
    assert(!api->set("source1",&defaults));
    assert(!api->get("source1",false,&current) && current.keys[0][1].usage==5);
    defaults.physical[1]=42;
    assert(api->set("source1",&defaults)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);defaults.physical[1]=50000;
    owner.prepare_error=ESP_ERR_NOT_SUPPORTED;
    assert(api->reset("source1")==SOLAR_OS_NATIVE_ERROR_NOT_SUPPORTED);
    assert(!api->get("source1",false,&current) && current.keys[0][1].usage==5);
    owner.prepare_error=ESP_OK;assert(!api->reset("source1"));
    file("{\"schema\":1,\"keys\":[{\"physical\":50000,\"usage\":6}]}");
    assert(!api->load("source1","/map.json"));
    assert(!api->get("source1",false,&current) && current.keys[0][1].usage==6);
    assert(!api->parse("{\"schema\":1,\"keys\":[{\"physical\":50000,\"usage\":7}]}",&current,&current));
    assert(current.keys[0][1].usage==7);
    assert(api->parse("{}",&current,&current)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && current.keys[0][1].usage==7);
    fail_allocation=allocations+1;
    assert(api->parse(contents,&current,&current)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && current.keys[0][1].usage==7);fail_allocation=0;
    solar_os_input_keymap_state_t state={0};solar_os_input_keymap_transition_t transition;
    assert(api->decode(&state,&current,900,true,&transition) && transition.modifiers==SOLAR_OS_INPUT_MOD_LEFT_SHIFT);
    assert(api->decode(&state,&current,50000,true,&transition) && transition.usage==7);
    state.caps_lock=true;api->release(&state);assert(state.caps_lock && !state.modifiers && !state.held[1]);
    bool published=false;unsigned priority;
    assert(!native_lock(&owner));
    assert(!api->provider_press_priority(profile,60000,&priority) && priority==0);
    assert(!api->provider_press_priority(profile,900,&priority) && priority==1);
    assert(!api->provider_process(profile,50000,true,&published) && published);
    assert(events[event_count-1].usage==6);
    assert(!api->provider_release_all(profile));native_unlock(&owner);
    unsigned unlocked_before=unlocked;
    atomic_store(&pause_lock,true);pthread_t thread;assert(!pthread_create(&thread,NULL,read_profile,NULL));
    while(!atomic_load(&entered))sched_yield();
    assert(api->provider_close(profile)==SOLAR_OS_NATIVE_ERROR_BUSY && live_adapters==1 && retains==releases+1);
    assert(api->get("source1",false,&defaults)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(api->provider_process(profile,50000,false,&published)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !published);
    assert(solar_os_native_handle_close_kind(producer,SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER)==SOLAR_OS_NATIVE_ERROR_BUSY);
    atomic_store(&continue_lock,true);assert(!pthread_join(thread,NULL));
    assert(!owner.locked && unlocked==unlocked_before+1); /* Admitted calls must still unlock. */
    assert(api->provider_close(producer)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->provider_close(profile) && !live_adapters && retains==releases);
    assert(api->provider_close(profile)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!solar_os_native_handle_close_kind(producer,SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER) && closed_sources==1);
    atomic_store(&pause_lock,false);descriptor.lock=native_lock;
    assert(!solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER,(void *)(uintptr_t)1,producer_destroy,&producer));
    descriptor.producer=producer;descriptor.defaults=&defaults;
    solar_os_native_handle_t handles[127];
    for(unsigned i=0;i<127;++i)assert(!solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_EXPR,&owner,producer_destroy,&handles[i]));
    assert(api->provider_open(&descriptor,&profile)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !profile && !live_adapters && retains==releases);
    for(unsigned i=0;i<127;++i)assert(!solar_os_native_handle_close_kind(handles[i],SOLAR_OS_NATIVE_HANDLE_EXPR));
    assert(!solar_os_native_handle_close_kind(producer,SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER));
    assert(!solar_os_native_handle_count());
    puts("native keymaps: real profiles, parsing, topology, copied callbacks, producer/code leases and quiescent retirement passed");
    return 0;
}
