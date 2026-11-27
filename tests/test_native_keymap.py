from pathlib import Path
import subprocess
import tempfile
import unittest
from host_cjson import CJSON
from test_keyboard_layouts import write_keyboard_layouts

ROOT = Path(__file__).resolve().parents[1]


class NativeKeymapTest(unittest.TestCase):
    def test_layer_taps_and_keyboard_layouts_through_real_typed_producers(self):
        fixture = (ROOT / 'tests/host/input_keymap_layout_test.c').read_text()
        fixture = fixture.replace('#include "input_test.c"',
            '#define solar_os_memory_calloc backend_memory_calloc\n'
            '#define solar_os_memory_free backend_memory_free\n'
            f'#include "{ROOT}/tests/host/input_test.c"\n'
            '#undef solar_os_memory_calloc\n#undef solar_os_memory_free')
        fixture = fixture.replace('int main(void)', 'int layout_checks(void)')
        fixture = fixture.replace('solar_os_input_keyboard_source_open("layout-tap", true, &keyboard)', 'abi_open(&keyboard)')
        fixture = fixture.replace('solar_os_input_source_close(keyboard)', 'abi_close(keyboard)')
        for before, after in (
                ('solar_os_input_keymap_register(', 'abi_register('),
                ('solar_os_input_keymap_unregister(', 'keymap->provider_close('),
                ('solar_os_input_keymap_activate(', 'keymap->provider_activate('),
                ('solar_os_input_keymap_process(', 'keymap->provider_process('),
                ('solar_os_input_keymap_release_all(', 'keymap->provider_release_all(')):
            fixture = fixture.replace(before, after)
        prefix = r'''
#include "solar_os_native_keymap.h"
#include "solar_os_native_keymap_abi.h"
#include "solar_os_native_input.h"
#include "solar_os_native_input_abi.h"
#include "solar_os_native_handles.h"
#include "solar_os_native_support.h"
#include "solar_os_input_keymap.h"
#include "solar_os.h"
static const solar_os_native_keymap_api_v1_t *keymap;
static const solar_os_native_input_api_v1_t *input;
static solar_os_native_handle_t producer;
static solar_os_input_keymap_ops_t owner_ops;
static unsigned leases,adapters;
static esp_err_t abi_open(solar_os_input_source_t *source);
static void abi_close(solar_os_input_source_t source);
static esp_err_t abi_register(solar_os_input_source_t source,const solar_os_input_keymap_t *defaults,
    const solar_os_input_keymap_ops_t *ops,solar_os_input_keymap_handle_t *out);
'''
        suffix = r'''
const solar_os_app_t *solar_os_application_descriptor(void) { return NULL; }
void *solar_os_memory_calloc(size_t n,size_t size,solar_os_memory_class_t kind,const char *tag)
{
    if(strcmp(tag,"native.input.keymap"))return backend_memory_calloc(n,size,kind,tag);
    assert(n==1 && kind==SOLAR_OS_MEMORY_EXTERNAL_PREFERRED);++adapters;return calloc(n,size);
}
void solar_os_memory_free(void *value) { if(value){assert(adapters);--adapters;free(value);} }
int solar_os_native_library_retain(uint32_t lease,uint32_t *out)
{ assert(lease==42);*out=43;++leases;return 0; }
int solar_os_native_library_release(uint32_t lease) { assert(lease==43 && leases);--leases;return 0; }
static int abi_lock(void *user) { (void)user;return solar_os_native_result(owner_ops.lock(owner_ops.context)); }
static void abi_unlock(void *user) { (void)user;owner_ops.unlock(owner_ops.context); }
static esp_err_t abi_open(solar_os_input_source_t *source)
{
    int result=input->producer_open("layout-tap",SOLAR_OS_INPUT_SOURCE_KEYBOARD,SOLAR_OS_INPUT_CAP_KEY_EVENTS,true,&producer);
    if(result)return ESP_FAIL;
    solar_os_input_source_info_t info;assert(!input->producer_info(producer,&info));*source=info.source;return ESP_OK;
}
static void abi_close(solar_os_input_source_t source)
{ solar_os_input_source_info_t info;assert(!input->producer_info(producer,&info) && info.source==source);assert(!input->producer_close(producer)); }
static esp_err_t abi_register(solar_os_input_source_t source,const solar_os_input_keymap_t *defaults,
    const solar_os_input_keymap_ops_t *ops,solar_os_input_keymap_handle_t *out)
{
    solar_os_input_source_info_t info;assert(!input->producer_info(producer,&info) && info.source==source);
    owner_ops=*ops;
    solar_os_native_keymap_provider_v1_t descriptor={.struct_size=sizeof(descriptor),.library_lease=42,
        .producer=producer,.defaults=defaults,.lock=abi_lock,.unlock=abi_unlock};
    assert(!keymap->provider_open(&descriptor,out));
    assert(input->producer_close(producer)==SOLAR_OS_NATIVE_ERROR_BUSY);return ESP_OK;
}
int main(void)
{
    keymap=solar_os_native_keymap_get_service("input.keymap",1,sizeof(*keymap));assert(keymap);
    input=solar_os_native_input_get_service("input",1,sizeof(*input));assert(input);
    assert(!layout_checks());assert(!leases && !adapters && !solar_os_native_handle_count());return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            write_keyboard_layouts(directory)
            (stage / 'freertos').mkdir()
            (stage / 'freertos/task.h').write_text('#pragma once\n#include "freertos/FreeRTOS.h"\n'
                'TickType_t xTaskGetTickCount(void);\nvoid vTaskDelay(TickType_t ticks);\n')
            source = stage / 'layout.c'
            source.write_text(prefix + fixture + suffix)
            binary = stage / 'layout'
            sources = ['solar_os_input.c', 'solar_os_keys.c', 'solar_os_input_composition.c', 'solar_os_native_input.c', 'solar_os_input_keymap.c',
                'solar_os_input_keymap_mapper.c', 'solar_os_input_keymap_json.c', 'solar_os_native_keymap.c', 'solar_os_native_handles.c']
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_SERVICE_INPUT_KEYMAP=1',
                '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1', '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-O1',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', str(stage), '-I', str(ROOT / 'tests/host/registry_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'), '-I', str(CJSON),
                str(source), *[str(ROOT / 'src/services' / name) for name in sources],
                str(CJSON / 'cJSON.c'), '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_real_profiles_values_parser_producer_lifetime_and_callback_retirement(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'freertos').mkdir()
            (stage / 'freertos/task.h').write_text('#pragma once\n#include "freertos/FreeRTOS.h"\n'
                'TickType_t xTaskGetTickCount(void);\nvoid vTaskDelay(TickType_t ticks);\n')
            binary = stage / 'keymap'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_SERVICE_INPUT_KEYMAP=1',
                '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-O1', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', str(stage), '-I', str(ROOT / 'tests/host/registry_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'), '-I', str(CJSON),
                str(ROOT / 'tests/host/native_keymap_test.c'), str(ROOT / 'src/services/solar_os_input_keymap.c'),
                str(ROOT / 'src/services/solar_os_input_keymap_mapper.c'), str(ROOT / 'src/services/solar_os_input_keymap_json.c'),
                str(ROOT / 'src/services/solar_os_native_keymap.c'), str(ROOT / 'src/services/solar_os_native_handles.c'),
                str(CJSON / 'cJSON.c'), '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_unavailable_service_has_no_backend_dependency(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            source = stage / 'absent.c'
            source.write_text('#include <assert.h>\n#include "solar_os_native_keymap.h"\n'
                'int main(void){assert(!solar_os_native_keymap_get_service("input.keymap",1,8));return 0;}\n')
            subprocess.run(['cc', '-std=c11', '-DSOLAR_OS_PACKAGE_SERVICE_INPUT_KEYMAP=0', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(source), str(ROOT / 'src/services/solar_os_native_keymap.c'), '-o', str(stage / 'absent')], check=True)
            subprocess.run([str(stage / 'absent')], check=True)
