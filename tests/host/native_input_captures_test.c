#include "solar_os_native_input.h"
#include "solar_os_native_input_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os.h"
#include "solar_os_log.h"
#define solar_os_memory_calloc backend_memory_calloc
#define solar_os_memory_free backend_memory_free
#define main legacy_input_checks
#include "input_test.c"
#undef main
#undef solar_os_memory_calloc
#undef solar_os_memory_free

static void *capture_allocations[128];
static unsigned live_captures;
static bool fail_capture_state;
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t kind,const char *tag)
{
    if(strcmp(tag,"native.input.capture"))return backend_memory_calloc(n,size,kind,tag);
    assert(n==1 && kind==SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);
    if(fail_capture_state)return NULL;
    void *p=calloc(n,size);assert(p);
    for(unsigned i=0;i<128;++i)if(!capture_allocations[i]){capture_allocations[i]=p;++live_captures;return p;}
    abort();
}
void solar_os_memory_free(void *p)
{
    for(unsigned i=0;i<128;++i)if(p && capture_allocations[i]==p){capture_allocations[i]=NULL;--live_captures;free(p);return;}
    backend_memory_free(p);
}
bool solar_os_shutdown_requested(void) { return false; }
const char *esp_err_to_name(esp_err_t error) { (void)error;return "test error"; }
esp_err_t solar_os_log_write(solar_os_log_level_t level,const char *tag,const char *format,...)
{ (void)level;(void)tag;(void)format;return ESP_OK; }
void solar_os_shell_io_capture_output(solar_os_shell_io_t *io,solar_os_context_t *ctx)
{ (void)io;(void)ctx; }
static const solar_os_native_input_api_v1_t *api;
static solar_os_native_handle_t app_source,app_capture;
static esp_err_t start(solar_os_context_t *ctx)
{
    assert(ctx);
    int result=api->producer_open("owned",SOLAR_OS_INPUT_SOURCE_KEYBOARD,SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&app_source);
    return result ? ESP_FAIL : api->keyboard_open("owned",false,&app_capture);
}
static const solar_os_app_t app={.name="capture",.app_class=SOLAR_OS_APP_CLASS_COMMAND,.start=start};
int main(void)
{
    assert(!solar_os_input_init());api=solar_os_native_input_get_service("input",1,sizeof(*api));assert(api);
    solar_os_native_handle_t keyboard,capture=99,other=99;
    assert(!api->producer_open("keyboard",SOLAR_OS_INPUT_SOURCE_KEYBOARD,SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&keyboard));
    fail_capture_state=true;
    assert(api->keyboard_open("keyboard",false,&capture)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !capture && !live_captures);
    fail_capture_state=false;
    assert(api->keyboard_open("missing",false,&capture)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !capture && !live_captures);
    assert(!api->keyboard_open("keyboard",false,&capture) && api->keyboard_claimed(capture) && live_captures==1);
    assert(api->keyboard_open("keyboard",false,&other)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !other && live_captures==1);
    solar_os_input_key_event_t event;bool reset;
    assert(!api->producer_write_key(keyboard,1,0x04,'a',0,SOLAR_OS_INPUT_KEY_PRESS));
    assert(!api->keyboard_read(capture,&event,&reset) && !reset && event.key=='a');
    assert(api->keyboard_read(capture,&event,&reset)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !event.key && !reset);
    for(unsigned i=0;i<100;++i)assert(!api->producer_write_key_tap(keyboard,0,0,'b',0));
    assert(!api->keyboard_read(capture,&event,&reset) && reset && !event.source);
    void *pin=solar_os_native_handle_acquire(capture,SOLAR_OS_NATIVE_HANDLE_INPUT_KEYBOARD_CAPTURE);assert(pin);
    assert(api->keyboard_close(capture)==SOLAR_OS_NATIVE_ERROR_BUSY);solar_os_native_handle_release(capture);
    assert(api->pointer_close(capture)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->producer_close(keyboard));
    assert(!api->keyboard_read(capture,&event,&reset) && reset && api->keyboard_claimed(capture));
    assert(!api->keyboard_close(capture) && !live_captures);
    assert(api->keyboard_read(capture,&event,&reset)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    assert(!api->producer_open("shortcuts",SOLAR_OS_INPUT_SOURCE_KEYBOARD,SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&keyboard));
    assert(!api->keyboard_open("shortcuts",true,&capture));
    solar_os_input_release_session_keyboard();assert(!api->keyboard_claimed(capture));
    assert(api->keyboard_read(capture,&event,&reset)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->keyboard_close(capture) && !api->producer_close(keyboard) && !live_captures);

    solar_os_native_handle_t pointer;
    assert(!api->producer_open("mouse",SOLAR_OS_INPUT_SOURCE_MOUSE,
        SOLAR_OS_INPUT_CAP_POINTER_RELATIVE|SOLAR_OS_INPUT_CAP_POINTER_BUTTONS,true,&pointer));
    fail_pointer_capture_allocation=true;
    assert(api->pointer_open("mouse",&capture)==SOLAR_OS_NATIVE_ERROR_NO_MEMORY && !capture && !live_captures);
    fail_pointer_capture_allocation=false;
    assert(!api->pointer_open("mouse",&capture) && live_captures==1);
    assert(api->pointer_open("mouse",&other)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE && !other && live_captures==1);
    now_us+=1000;
    const solar_os_input_pointer_sample_t sample={.event={.mode=SOLAR_OS_INPUT_POINTER_RELATIVE,.action=SOLAR_OS_INPUT_POINTER_MOVE,.delta_x=7},.time_ms=(uint32_t)(now_us/1000),.modifiers=SOLAR_OS_INPUT_MOD_LEFT_ALT,.modifiers_valid=true};
    assert(!api->producer_write_pointer_sample(pointer,&sample));
    solar_os_input_pointer_sample_t output;
    assert(!api->pointer_read(capture,&output,&reset) && !reset && output.event.delta_x==7 && output.modifiers_valid);
    assert(api->pointer_read(capture,&output,&reset)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND && !output.event.source);
    assert(!api->producer_close(pointer));
    assert(api->pointer_read(capture,&output,&reset)==SOLAR_OS_NATIVE_ERROR_INVALID_STATE);
    assert(!api->pointer_close(capture) && !live_captures && !solar_os_native_handle_count());
    solar_os_context_t ctx={0};assert(!solar_os_app_start(&app,&ctx) && live_captures==1);
    assert(solar_os_app_stop_checked(&app,&ctx) && !live_captures && !solar_os_native_handle_count() && !api->source_count());
    assert(api->keyboard_close(app_capture)==SOLAR_OS_NATIVE_ERROR_NOT_FOUND);
    return 0;
}
