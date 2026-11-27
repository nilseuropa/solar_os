#include <pthread.h>
#define main legacy_input_checks
#include "input_test.c"
#undef main

static struct { esp_err_t result; solar_os_input_source_t source; } racers[2];
static void *create_source(void *argument)
{
    const size_t index=(uintptr_t)argument;
    racers[index].result=solar_os_input_source_create("race",SOLAR_OS_INPUT_SOURCE_KEYBOARD,
        SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&racers[index].source);
    return NULL;
}
int main(void)
{
    assert(!solar_os_input_init());
    solar_os_input_source_t builtin,owned,alias;
    assert(!solar_os_input_keyboard_source_open("builtin",true,&builtin));
    alias=99;
    assert(solar_os_input_source_create("builtin",SOLAR_OS_INPUT_SOURCE_KEYBOARD,
        SOLAR_OS_INPUT_CAP_KEY_EVENTS,false,&alias)==ESP_ERR_INVALID_STATE && !alias);
    solar_os_input_source_info_t info;
    assert(solar_os_input_source_get_info(builtin,&info) && info.ready);
    /* Existing driver reopen semantics still update that driver's readiness. */
    assert(!solar_os_input_keyboard_source_open("builtin",false,&alias) && alias==builtin);
    assert(solar_os_input_source_get_info(builtin,&info) && !info.ready);
    solar_os_input_source_close(builtin);
    assert(!solar_os_input_source_create("owned",SOLAR_OS_INPUT_SOURCE_KEYBOARD,
        SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&owned));
    assert(solar_os_input_keyboard_source_open("owned",false,&alias)==ESP_ERR_INVALID_STATE);
    assert(solar_os_input_source_get_info(owned,&info) && info.ready);
    alias=99;
    assert(solar_os_input_source_create("owned",SOLAR_OS_INPUT_SOURCE_KEYBOARD,
        SOLAR_OS_INPUT_CAP_KEY_EVENTS,false,&alias)==ESP_ERR_INVALID_STATE && !alias);
    assert(!solar_os_input_set_key(owned,1,0x04,'a',0));
    solar_os_input_key_event_t events[8];assert(solar_os_input_read_events(events,8)==1);
    solar_os_input_source_close(owned);
    assert(!solar_os_input_source_create("owned",SOLAR_OS_INPUT_SOURCE_KEYBOARD,
        SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&alias) && alias!=owned);
    assert(solar_os_input_read_events(events,8)==1 && events[0].source==owned && events[0].action==SOLAR_OS_INPUT_KEY_RELEASE);
    solar_os_input_source_close(alias);
    pthread_t threads[2];
    assert(!pthread_create(&threads[0],NULL,create_source,(void *)0));
    assert(!pthread_create(&threads[1],NULL,create_source,(void *)1));
    assert(!pthread_join(threads[0],NULL) && !pthread_join(threads[1],NULL));
    unsigned created=0;
    for(unsigned i=0;i<2;++i) {
        if(racers[i].result==ESP_OK){++created;solar_os_input_source_close(racers[i].source);}
        else assert(racers[i].result==ESP_ERR_INVALID_STATE && !racers[i].source);
    }
    assert(created==1 && !solar_os_input_source_count());
    alias=99;
    assert(solar_os_input_source_create(NULL,SOLAR_OS_INPUT_SOURCE_KEYBOARD,
        SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&alias)==ESP_ERR_INVALID_ARG && !alias);
    return 0;
}
