#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include "solar_os_native_input.h"
#include "solar_os_native_input_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_input_composition.h"
#include "solar_os.h"
#define main baseline_input_checks
#include "input_test.c"
#undef main

const solar_os_app_t *solar_os_application_descriptor(void) { return NULL; }
static const solar_os_native_input_api_v1_t *api;
static solar_os_native_handle_t producers[4];
static const uint8_t masks[4]={SOLAR_OS_INPUT_MOD_LEFT_ALT,SOLAR_OS_INPUT_MOD_LEFT_SHIFT,
    SOLAR_OS_INPUT_MOD_LEFT_CTRL,SOLAR_OS_INPUT_MOD_RIGHT_SHIFT};
static atomic_uint ready;
static atomic_bool begin;
static void *produce(void *value)
{
    unsigned i=(unsigned)(uintptr_t)value;
    assert(!api->producer_modifiers(producers[i],masks[i]));atomic_fetch_add(&ready,1);
    while(!atomic_load(&begin))sched_yield();
    for(unsigned j=0;j<2000;++j){
        assert(!api->producer_modifiers(producers[i],j%2 ? masks[i] : 0));
        assert(!(api->composition_modifiers() & ~(masks[0]|masks[1]|masks[2]|masks[3])));
    }
    assert(!api->producer_modifiers(producers[i],masks[i]));return NULL;
}
static int dummy_destroy(void *value) { assert(value);return 0; }
int main(void)
{
    assert(!solar_os_input_init());api=solar_os_native_input_get_service("input",1,sizeof(*api));assert(api);
    assert(api->composition_apply==solar_os_input_composition_apply);
    assert(api->composition_apply_with_modifiers==solar_os_input_composition_apply_with_modifiers);
    assert(api->key_name==solar_os_key_name && api->parse_key==solar_os_key_parse);
    uint8_t key;
    assert(api->parse_key("PAGE_UP",&key) && key==SOLAR_OS_KEY_PAGE_UP);
    assert(!strcmp(api->key_name(SOLAR_OS_KEY_UP),"UP"));
    assert(!api->parse_key("invalid-key",&key));
    assert(api->composition_apply_with_modifiers(SOLAR_OS_KEY_UP,SOLAR_OS_INPUT_MOD_LEFT_ALT)==SOLAR_OS_KEY_PAGE_UP);
    assert(api->composition_apply_with_modifiers(SOLAR_OS_KEY_UP,SOLAR_OS_INPUT_MOD_LEFT_ALT|SOLAR_OS_INPUT_MOD_LEFT_CTRL)==SOLAR_OS_KEY_UP);
    assert(api->composition_apply_with_modifiers(SOLAR_OS_KEY_LEFT,SOLAR_OS_INPUT_MOD_LEFT_SHIFT)==SOLAR_OS_KEY_HOME);
    for(unsigned i=0;i<4;++i){char name[16];snprintf(name,sizeof(name),"producer%u",i);
        assert(!api->producer_open(name,SOLAR_OS_INPUT_SOURCE_KEYBOARD,SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&producers[i]));}
    assert(!api->producer_modifiers(producers[0],masks[0]));
    assert(api->composition_apply(SOLAR_OS_KEY_UP)==SOLAR_OS_KEY_PAGE_UP);
    assert(!api->producer_modifiers(producers[1],masks[1]));
    assert(api->composition_modifiers()==(masks[0]|masks[1]));
    assert(!api->producer_release_all(producers[0]) && api->composition_modifiers()==masks[1]);
    solar_os_native_handle_t wrong;assert(!solar_os_native_handle_create(SOLAR_OS_NATIVE_HANDLE_EXPR,&ready,dummy_destroy,&wrong));
    assert(api->producer_modifiers(wrong,255)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && api->composition_modifiers()==masks[1]);
    assert(!solar_os_native_handle_close_kind(wrong,SOLAR_OS_NATIVE_HANDLE_EXPR));
    pthread_t workers[4];
    for(unsigned i=0;i<4;++i)assert(!pthread_create(&workers[i],NULL,produce,(void *)(uintptr_t)i));
    while(atomic_load(&ready)!=4)sched_yield();
    uint8_t expected=masks[0]|masks[1]|masks[2]|masks[3];assert(api->composition_modifiers()==expected);
    atomic_store(&begin,true);for(unsigned i=0;i<4;++i)assert(!pthread_join(workers[i],NULL));
    assert(api->composition_modifiers()==expected);
    for(unsigned i=0;i<4;++i){
        assert(!api->producer_close(producers[i]));expected&=(uint8_t)~masks[i];assert(api->composition_modifiers()==expected);
        assert(api->producer_modifiers(producers[i],255)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && api->composition_modifiers()==expected);
    }
    assert(!api->composition_modifiers() && !solar_os_native_handle_count());
    assert(api->composition_apply(SOLAR_OS_KEY_UP)==SOLAR_OS_KEY_UP);
    solar_os_input_source_t raw;
    assert(!solar_os_input_keyboard_source_open("raw",true,&raw));
    solar_os_input_composition_set_modifiers(raw,SOLAR_OS_INPUT_MOD_LEFT_ALT);
    assert(api->composition_modifiers()==SOLAR_OS_INPUT_MOD_LEFT_ALT);
    solar_os_input_source_release_all(raw);assert(!api->composition_modifiers());
    solar_os_input_composition_set_modifiers(raw,SOLAR_OS_INPUT_MOD_LEFT_ALT);
    solar_os_input_source_close(raw);assert(!api->composition_modifiers());
    assert(!solar_os_input_keyboard_source_open("reused",true,&raw) && !api->composition_modifiers());
    solar_os_input_source_close(raw);
    puts("native input composition: board chords, Ctrl precedence, independent producer masks, synchronized snapshots, release and source cleanup passed");
    return 0;
}
