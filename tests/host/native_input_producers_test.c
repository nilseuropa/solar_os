#include "solar_os_native_input.h"
#include "solar_os_native_input_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os.h"
#include "solar_os_log.h"
#define main legacy_input_checks
#include "input_test.c"
#undef main

bool solar_os_shutdown_requested(void) { return false; }
const char *esp_err_to_name(esp_err_t error) { (void)error;return "test error"; }
esp_err_t solar_os_log_write(solar_os_log_level_t level,const char *tag,const char *format,...)
{ (void)level;(void)tag;(void)format;return ESP_OK; }
void solar_os_shell_io_capture_output(solar_os_shell_io_t *io,solar_os_context_t *ctx)
{ (void)io;(void)ctx; }
static const solar_os_native_input_api_v1_t *api;
static solar_os_native_handle_t owned;
static esp_err_t start(solar_os_context_t *ctx)
{
    assert(ctx);
    return api->producer_open("owned",SOLAR_OS_INPUT_SOURCE_KEYBOARD,SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&owned);
}
static const solar_os_app_t app={.name="producer",.app_class=SOLAR_OS_APP_CLASS_COMMAND,.start=start};
static int dummy_destroy(void *state) { assert(state);return SOLAR_OS_NATIVE_OK; }
int main(void)
{
    assert(!solar_os_input_init());
    api=solar_os_native_input_get_service("input",1,sizeof(*api));assert(api);
    solar_os_native_handle_t handle=999;
    assert(api->producer_open(NULL,SOLAR_OS_INPUT_SOURCE_KEYBOARD,SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&handle)==SOLAR_OS_NATIVE_ERROR_INVALID_ARGUMENT && !handle);
    assert(!api->producer_open("keys",SOLAR_OS_INPUT_SOURCE_KEYBOARD,SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&handle));
    assert(!pointer_queue_allocations && !axis_queue_allocations && !gesture_queue_allocations);
    solar_os_input_source_info_t info;
    assert(!api->producer_info(handle,&info) && !strcmp(info.name,"keys"));
    assert(api->producer_open("keys",SOLAR_OS_INPUT_SOURCE_KEYBOARD,SOLAR_OS_INPUT_CAP_KEY_EVENTS,false,&owned)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !owned);
    assert(!api->producer_write_key(handle,1,0x04,'a',0,SOLAR_OS_INPUT_KEY_PRESS));
    solar_os_input_key_event_t events[8];assert(solar_os_input_read_events(events,8)==1 && events[0].source==info.source && events[0].key=='a');
    assert(!api->producer_set_key(handle,1,0x05,'b',0));
    assert(solar_os_input_read_events(events,8)==2 && events[0].action==SOLAR_OS_INPUT_KEY_RELEASE && events[1].key=='b');
    assert(!api->producer_release_all(handle));
    assert(solar_os_input_read_events(events,8)==1 && events[0].action==SOLAR_OS_INPUT_KEY_RELEASE);
    assert(!api->producer_write_key_tap(handle,0,0,'c',0));
    assert(solar_os_input_read_events(events,8)==2 && events[0].key=='c' && events[1].action==SOLAR_OS_INPUT_KEY_RELEASE);
    assert(!api->producer_write_char(handle,'d'));
    char characters[8];assert(api->producer_read_chars(handle,characters,sizeof(characters))==1 && characters[0]=='d');
    assert(!api->producer_ready(handle,false) && !api->keyboard_count());
    assert(!api->producer_ready(handle,true) && api->keyboard_count()==1);
    void *p=solar_os_native_handle_acquire(handle,SOLAR_OS_NATIVE_HANDLE_INPUT_PRODUCER);assert(p);
    assert(api->producer_close(handle)==SOLAR_OS_NATIVE_ERROR_BUSY);solar_os_native_handle_release(handle);
    const solar_os_native_handle_t stale=handle;assert(!api->producer_close(handle));
    assert(api->producer_write_char(stale,'x')==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    solar_os_native_handle_t pointer;
    assert(!api->producer_open("touch",SOLAR_OS_INPUT_SOURCE_TOUCH,
        SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE|SOLAR_OS_INPUT_CAP_POINTER_BUTTONS,true,&pointer));
    assert(!api->producer_info(pointer,&info) && (info.capabilities&SOLAR_OS_INPUT_CAP_GESTURE_EVENTS));
    solar_os_input_pointer_event_t point={.mode=SOLAR_OS_INPUT_POINTER_ABSOLUTE,.action=SOLAR_OS_INPUT_POINTER_MOVE,.x=11,.y=12};
    assert(!api->producer_write_pointer(pointer,&point));
    solar_os_input_pointer_event_t received;assert(solar_os_input_read_pointer_events(&received,1)==1 && received.x==11 && received.source==info.source);
    const solar_os_input_pointer_sample_t sample={.event=point,.time_ms=1234,.modifiers=SOLAR_OS_INPUT_MOD_LEFT_ALT,.modifiers_valid=true};
    assert(!api->producer_write_pointer_sample(pointer,&sample) && solar_os_input_read_pointer_events(&received,1)==1);
    const solar_os_input_pointer_calibration_t calibration={.min_x=0,.max_x=100,.min_y=0,.max_y=100,.width=101,.height=101};
    assert(!api->producer_calibration_default(pointer,&calibration) && !nvs_blob_length);
    assert(!api->producer_gestures(pointer,0));
    solar_os_input_gesture_event_t gesture={.gesture=SOLAR_OS_INPUT_GESTURE_WAVE,.direction=SOLAR_OS_INPUT_GESTURE_DIRECTION_EAST};
    assert(!api->producer_write_gesture(pointer,&gesture));
    assert(solar_os_input_read_gesture_events(&gesture,1)==1 && gesture.source==info.source);
    assert(!api->producer_close(pointer));
    solar_os_native_handle_t axes;
    assert(!api->producer_open("axes",SOLAR_OS_INPUT_SOURCE_JOYSTICK,SOLAR_OS_INPUT_CAP_AXIS_EVENTS,true,&axes));
    solar_os_input_axis_event_t axis={.axis=SOLAR_OS_INPUT_AXIS_X,.value=123,.delta=7};
    assert(!api->producer_write_axis(axes,&axis));
    assert(solar_os_input_read_axis_events(&axis,1)==1 && axis.value==123);
    axis.value=0;axis.delta=-123;
    assert(!api->producer_write_axis(axes,&axis));
    assert(!api->producer_close(axes));
    assert(solar_os_input_read_axis_events(&axis,1)==1 && !axis.value);
    assert(!api->source_count() && !solar_os_native_handle_count());
    /* No adapter allocations; a full handle pool rolls back the core source. */
    uint32_t dummy=1;solar_os_native_handle_t full[128];
    for(unsigned i=0;i<128;++i) assert(!solar_os_native_handle_create(199,&dummy,dummy_destroy,full+i));
    assert(api->producer_open("rollback",SOLAR_OS_INPUT_SOURCE_KEYBOARD,SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&handle)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !handle && !api->source_count());
    assert(api->producer_info(full[0],&info)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !info.source);
    for(unsigned i=0;i<128;++i) assert(!solar_os_native_handle_close(full[i]));
    solar_os_context_t ctx={0};assert(!solar_os_app_start(&app,&ctx) && owned && api->source_count()==1);
    assert(!api->producer_modifiers(owned,SOLAR_OS_INPUT_MOD_LEFT_ALT));
    assert(!api->producer_set_key(owned,1,0x04,'a',0));
    assert(solar_os_input_read_events(events,8)==1);
    solar_os_app_suspend(&app,&ctx);solar_os_app_resume(&app,&ctx);assert(api->source_count()==1 && api->composition_modifiers()==SOLAR_OS_INPUT_MOD_LEFT_ALT);
    assert(solar_os_app_stop_checked(&app,&ctx) && !api->source_count() && !solar_os_native_handle_count());
    assert(!api->composition_modifiers());
    assert(solar_os_input_read_events(events,8)==1 && events[0].action==SOLAR_OS_INPUT_KEY_RELEASE);
    assert(api->producer_close(owned)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    return 0;
}
