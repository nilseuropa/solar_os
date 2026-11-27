#include "solar_os_native_input.h"
#include "solar_os.h"
#include "solar_os_native_input_abi.h"
#define main input_baseline_checks
#include "input_test.c"
#undef main

const solar_os_app_t *solar_os_application_descriptor(void) { return NULL; }

int main(void)
{
    assert(!solar_os_input_init());
    const solar_os_native_input_api_v1_t *api=solar_os_native_input_get_service("input",1,8);
    assert(api && api->struct_size==sizeof(*api));
    assert(!solar_os_native_input_get_service("input",2,8));
    assert(!solar_os_native_input_get_service("input",1,sizeof(*api)+1));
    assert(!solar_os_native_input_get_service("missing",1,8));
    assert(api->get_pressed==solar_os_input_get_pressed && api->source_find==solar_os_input_source_find);
    solar_os_input_source_t keyboard,pointer;
    assert(!solar_os_input_keyboard_source_open("abi-kbd",true,&keyboard));
    assert(!solar_os_input_touch_source_open("abi-touch",&pointer));
    assert(api->keyboard_count()==1 && api->source_count()==2);
    solar_os_input_source_info_t info;
    assert(api->source_find("abi-kbd",&info) && info.source==keyboard && info.ready);
    assert(api->source_get(0,&info) && !strcmp(info.name,"abi-kbd"));
    assert(api->source_get_info(pointer,&info) && info.source_class==SOLAR_OS_INPUT_SOURCE_TOUCH);
    uint32_t generation=api->key_press_generation();
    assert(!solar_os_input_write_key(keyboard,1,0x04,'A',SOLAR_OS_INPUT_MOD_LEFT_SHIFT,SOLAR_OS_INPUT_KEY_PRESS));
    assert(api->key_press_generation()!=generation);
    solar_os_input_key_event_t pressed[4];assert(api->get_pressed(pressed,4)==1 && pressed[0].key=='A');
    solar_os_input_key_event_t queued[8];
    const size_t count=solar_os_input_read_events(queued,8);
    bool found=false;
    for(size_t i=0;i<count;++i) if(queued[i].key=='A') found=true;
    assert(found); /* Observation does not consume, including modifier events. */
    solar_os_input_source_diagnostics_t diagnostics;
    assert(api->source_get_diagnostics(keyboard,&diagnostics) && diagnostics.has_key && diagnostics.last_key.key=='A');
    assert(!api->set_keyboard_layout(SOLAR_OS_INPUT_KEYBOARD_LAYOUT_DE));
    assert(api->translate_hid_usage_byte(0x1c,0,false)=='z');
    assert(!api->set_keyboard_layout_list("us,de"));
    char list[32];assert(!api->keyboard_layout_list(list,sizeof(list)) && !strcmp(list,"us,de"));
    uint8_t key,modifiers;assert(api->parse_key_chord("ALT+RIGHT",&key,&modifiers));
    assert(key==SOLAR_OS_KEY_RIGHT && modifiers==SOLAR_OS_INPUT_MOD_LEFT_ALT);
    uint16_t rate,delay;assert(!api->set_repeat(17,400));api->get_repeat(&rate,&delay);assert(rate==17 && delay==400);
    assert(!api->set_repeat(0,400));api->get_repeat(&rate,&delay);assert(!rate && delay==400);
    assert(api->set_repeat(SOLAR_OS_INPUT_REPEAT_RATE_MAX+1,400)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    solar_os_input_pointer_calibration_t calibration={.min_x=10,.max_x=110,.min_y=20,.max_y=220,.width=101,.height=201};
    assert(!api->pointer_calibration_set(pointer,&calibration));
    bool enabled;solar_os_input_pointer_calibration_t result;
    assert(!api->pointer_calibration_get(pointer,&enabled,&result) && enabled && !memcmp(&result,&calibration,sizeof(result)));
    assert(!api->pointer_calibration_reset(pointer));
    solar_os_input_pointer_event_t event={.mode=SOLAR_OS_INPUT_POINTER_ABSOLUTE,.x=10,.y=20};
    assert(!api->pointer_apply_orientation(&event,100,200,90) && event.x==20 && event.y==89);
    assert(api->pointer_apply_orientation(&event,100,200,45)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    solar_os_input_text_t text;
    assert(!api->key_text('a',&text) && text.codepoint=='a' && !text.dead);
    solar_os_input_compose_t compose={0};uint16_t output[2]={0};
    assert(api->compose_feed(&compose,&text,output)==1 && output[0]=='a');
    assert(!api->compose_feed(&compose,NULL,output));
    assert(api->key_text('a',NULL)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT);
    assert(api->resolve_layout_key(NULL,&text)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !text.codepoint);
    solar_os_input_source_close(keyboard);solar_os_input_source_close(pointer);
    solar_os_input_init(); /* The core releases any retired queues after the test. */
    return 0;
}
