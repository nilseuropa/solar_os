#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../src/jobs/solar_os_screen_keyboard_job.c"

static solar_os_input_key_event_t emitted;
static unsigned emissions, releases, clears, owner_calls, closes, route_releases;
esp_err_t solar_os_input_write_key_tap(solar_os_input_source_t source,uint16_t physical,
    uint16_t usage,uint8_t key,uint8_t mods)
{
    emitted=(solar_os_input_key_event_t){.source=source,.physical_key=physical,.usage=usage,.key=key,.modifiers=mods};
    emissions++;return ESP_OK;
}
void solar_os_input_source_release_all(solar_os_input_source_t source) {assert(source==3);releases++;}
esp_err_t solar_os_input_route_update(const void *owner,const solar_os_input_route_config_t *config)
{assert(owner==keyboard && config);return ESP_OK;}
esp_err_t solar_os_display_clear_layer(const char *name,const char *owner)
{assert(!strcmp(name,"display0") && !strcmp(owner,"screen-keyboard"));clears++;return ESP_OK;}
void solar_os_memory_free(void *p) {free(p);}
void solar_os_input_route_release(const void *owner) {assert(owner==keyboard);route_releases++;}
void solar_os_input_source_close(solar_os_input_source_t source) {assert(source==3);closes++;}
esp_err_t solar_os_sessions_run_on_owner(esp_err_t (*operation)(void *),void *context)
{owner_calls++;return operation(context);}

int main(void)
{
    const solar_os_screen_keyboard_layout_t *layout=solar_os_screen_keyboard_layout_find("grid");
    screen_keyboard_t state={.layout=layout,.route={.target="display0",.excluded_source=3}};
    state.layout_state=calloc(1,layout->state_size);keyboard=&state;
    layout->init(state.layout_state,false);
    apply(&state,(solar_os_screen_keyboard_action_t){.type=SOLAR_OS_SCREEN_KEYBOARD_REDRAW});
    assert(state.dirty && !emissions);state.dirty=false;
    solar_os_screen_keyboard_action_t letter={.type=SOLAR_OS_SCREEN_KEYBOARD_EMIT,.key='a',.shifted='A',.usage=4};
    state.modifiers=SOLAR_OS_INPUT_MOD_LEFT_CTRL|SOLAR_OS_INPUT_MOD_LEFT_SHIFT;
    apply(&state,letter);
    assert(emissions==1 && emitted.source==3 && emitted.key=='A' && emitted.usage==4);
    assert(emitted.modifiers==(SOLAR_OS_INPUT_MOD_LEFT_CTRL|SOLAR_OS_INPUT_MOD_LEFT_SHIFT) && !state.modifiers);
    state.caps=true;apply(&state,letter);
    assert(emitted.key=='A' && (emitted.modifiers&SOLAR_OS_INPUT_MOD_SHIFT));
    state.modifiers=SOLAR_OS_INPUT_MOD_LEFT_SHIFT;apply(&state,letter);
    assert(emitted.key=='a' && !(emitted.modifiers&SOLAR_OS_INPUT_MOD_SHIFT));
    layout->init(state.layout_state,true);(void)layout->resize(state.layout_state,400,240);
    (void)layout->key(state.layout_state,SOLAR_OS_KEY_RIGHT);
    apply(&state,layout->key(state.layout_state,'\r'));
    assert(emitted.key=='+' && emitted.usage==0x2e && (emitted.modifiers&SOLAR_OS_INPUT_MOD_SHIFT));
    state.visible=state.auto_hide=true;state.modifiers=SOLAR_OS_INPUT_MOD_LEFT_ALT;
    apply(&state,letter);
    assert(!state.visible && !state.caps && !state.modifiers && releases==1 && clears==1);
    screen_keyboard_t *heap=malloc(sizeof(*heap));*heap=state;keyboard=heap;
    heap->visible=true;
    stop(NULL);
    assert(!keyboard && owner_calls==1 && closes==1 && route_releases==1 && releases==2 && clears==2);
    puts("screen keyboard emission and lifecycle tests: ok");
}
