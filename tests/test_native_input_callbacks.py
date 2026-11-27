from pathlib import Path
import subprocess
import tempfile
import unittest
from test_keyboard_layouts import write_keyboard_layouts

ROOT = Path(__file__).resolve().parents[1]


class NativeInputCallbacksTest(unittest.TestCase):
    def test_real_registry_retirement_code_leases_self_close_and_failure_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            write_keyboard_layouts(directory)
            binary = Path(directory) / 'input'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1',
                '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-O1', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', directory, '-I', str(ROOT / 'tests/host/registry_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_input_callbacks_test.c'), str(ROOT / 'src/services/solar_os_input.c'),
                str(ROOT / 'src/services/solar_os_keys.c'), str(ROOT / 'src/services/solar_os_input_composition.c'), str(ROOT / 'src/services/solar_os_native_input.c'),
                str(ROOT / 'src/services/solar_os_native_handles.c'),
                '-Wl,--wrap=solar_os_input_gesture_observer_register', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_registration_holds_real_library_image_until_quiescent_close(self):
        from test_native_libraries import write_runtime_support
        fixture = (ROOT / 'tests/host/native_libraries_test.c').read_text()
        fixture = fixture.replace('int main(void)', 'int original_library_checks(void)')
        fixture = fixture.replace('../../src/services/solar_os_native_runtime.c',
            str(ROOT / 'src/services/solar_os_native_runtime.c'))
        fixture += r'''
#include "solar_os_native_input.h"
#include "solar_os_native_input_abi.h"
#include "solar_os_input.h"
#include "solar_os.h"
#include "nvs.h"
const solar_os_app_t *solar_os_application_descriptor(void) { return NULL; }
int64_t esp_timer_get_time(void) { return 0; }
size_t strlcpy(char *out,const char *in,size_t capacity)
{ size_t n=strlen(in);if(capacity){size_t copy=n<capacity?n:capacity-1;memcpy(out,in,copy);out[copy]=0;}return n; }
esp_err_t nvs_open(const char *name,nvs_open_mode_t mode,nvs_handle_t *out)
{ (void)name;(void)mode;*out=1;return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_get_u16(nvs_handle_t h,const char *key,uint16_t *out)
{ (void)h;(void)key;(void)out;return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_u16(nvs_handle_t h,const char *key,uint16_t value)
{ (void)h;(void)key;(void)value;return ESP_OK; }
esp_err_t nvs_get_blob(nvs_handle_t h,const char *key,void *out,size_t *size)
{ (void)h;(void)key;(void)out;(void)size;return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_set_blob(nvs_handle_t h,const char *key,const void *in,size_t size)
{ (void)h;(void)key;(void)in;(void)size;return ESP_OK; }
esp_err_t nvs_erase_key(nvs_handle_t h,const char *key)
{ (void)h;(void)key;return ESP_ERR_NVS_NOT_FOUND; }
esp_err_t nvs_commit(nvs_handle_t h) { (void)h;return ESP_OK; }
void nvs_close(nvs_handle_t h) { (void)h; }
static unsigned observed;
static void from_library(const solar_os_input_gesture_event_t *event,void *user)
{ assert(event && user==&images && images==1);++observed; }
int main(void)
{
    uint32_t lease;const void *library_api;acquire(&lease,&library_api);
    assert(!solar_os_input_init());
    const solar_os_native_input_api_v1_t *input=solar_os_native_input_get_service("input",1,sizeof(*input));assert(input);
    solar_os_input_source_t source;
    assert(!solar_os_input_source_open_typed("events",SOLAR_OS_INPUT_SOURCE_GESTURE,SOLAR_OS_INPUT_CAP_GESTURE_EVENTS,true,&source));
    const solar_os_native_input_observer_v1_t descriptor={.struct_size=sizeof(descriptor),.library_lease=lease,.observer=from_library,.user=&images};
    solar_os_native_handle_t observer;assert(!input->observer_open(&descriptor,&observer));
    assert(!solar_os_native_library_release(lease));
    assert(solar_os_native_library_unload("example")==SOLAR_OS_NATIVE_ERROR_INVALID_STATE && images==1);
    const solar_os_input_gesture_event_t event={.gesture=SOLAR_OS_INPUT_GESTURE_FLICK};
    assert(!solar_os_input_write_gesture(source,&event) && observed==1);
    assert(!input->observer_close(observer));solar_os_input_source_close(source);
    assert(!solar_os_native_library_unload("example") && !images && !allocations && !native_library_leases);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            write_runtime_support(stage)
            write_keyboard_layouts(directory)
            source = stage / 'library_input.c'
            source.write_text(fixture)
            binary = stage / 'library-input'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1',
                '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-O1', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', directory, '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(source), str(ROOT / 'src/services/solar_os_input.c'), str(ROOT / 'src/services/solar_os_keys.c'), str(ROOT / 'src/services/solar_os_input_composition.c'),
                str(ROOT / 'src/services/solar_os_native_input.c'), str(ROOT / 'src/services/solar_os_native_handles.c'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
