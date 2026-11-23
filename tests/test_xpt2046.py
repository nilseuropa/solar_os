from pathlib import Path
import subprocess
import tempfile
import unittest

from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class Xpt2046Test(unittest.TestCase):
    def compile_run(self, code, sources=()):
        with tempfile.TemporaryDirectory() as temp:
            path, binary = Path(temp, 'xpt2046.c'), Path(temp, 'xpt2046')
            path.write_text(code)
            subprocess.run([
                'cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-O1', '-g', '-include', str(ROOT / 'tests/host/compat.h'),
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                '-I' + str(ROOT / 'tests/host/epd_stubs'),
                '-I' + str(ROOT / 'tests/host/expansion_stubs'),
                '-I' + str(ROOT / 'tests/host'), '-I' + str(ROOT / 'include'),
                '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/services'),
                '-I' + str(ROOT / 'src/drivers'), '-I' + str(ROOT / 'components/u8g2/src/clib'),
                str(path), *map(str, sources), '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_adc_filtering_pressure_and_powerdown(self):
        self.compile_run((ROOT / 'tests/host/xpt2046_protocol_test.inc').read_text(),
                         (ROOT / 'src/drivers/xpt2046.c',))

    def test_expansion_lifetime_routing_and_failures(self):
        service = (ROOT / 'src/services/solar_os_xpt2046.c').read_text()
        fixture = (ROOT / 'tests/host/xpt2046_expansion_test.inc').read_text()
        self.compile_run(fixture.replace('/* SERVICE */', service[service.index('typedef struct xpt_device'):]))

    def test_python_lua_display_binding_names_are_not_truncated(self):
        functions = production_functions((ROOT / 'src/apps/solar_os_python.c').read_text(),
                                         ('python_expansion_key_known', 'python_expansion_add_binding'))
        functions += production_functions((ROOT / 'src/apps/solar_os_lua.c').read_text(),
                                          ('solua_expansion_key_known', 'solua_expansion_add_binding'))
        fixture = r'''
#include <assert.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include "solar_os_expansion.h"
typedef struct { int unused; } lua_State;
static jmp_buf error;
#define MP_ERROR_TEXT(text) text
static void mp_raise_ValueError(const char *message) { (void)message; longjmp(error,1); }
static int luaL_error(lua_State *L, const char *message) { (void)L; (void)message; longjmp(error,1); }
size_t strlcpy(char *out,const char *in,size_t n) { size_t len=strlen(in);if(n){size_t copy=len<n?len:n-1;memcpy(out,in,copy);out[copy]=0;}return len; }
/* FUNCTIONS */
int main(void) {
    const char *keys[]={"display","pressure","swap_xy","invert_x","invert_y"};
    for(size_t i=0;i<5;i++)assert(python_expansion_key_known(keys[i]) && solua_expansion_key_known(keys[i]));
    solar_os_expansion_binding_t bindings[24]={0};size_t count=0;
    python_expansion_add_binding(bindings,&count,SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET,"display","lcd0",-1,-1);
    solua_expansion_add_binding(NULL,bindings,&count,SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET,"display","lcd1",-1,-1);
    assert(count==2 && !strcmp(bindings[0].target,"lcd0") && !strcmp(bindings[1].target,"lcd1"));
    if(!setjmp(error)) { python_expansion_add_binding(bindings,&count,SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET,"display","1234567890123456",-1,-1); assert(false); }
    assert(count==2);
    if(!setjmp(error)) { solua_expansion_add_binding(NULL,bindings,&count,SOLAR_OS_EXPANSION_BINDING_DISPLAY_TARGET,"display","1234567890123456",-1,-1); assert(false); }
    assert(count==2);
    puts("XPT2046 scripting: config keys, named display bindings and overlong-name rejection passed");
}
'''
        self.compile_run(fixture.replace('/* FUNCTIONS */', functions))

    def test_default_calibration_preserves_saved_values(self):
        source = (ROOT / 'src/services/solar_os_input.c').read_text()
        functions = production_functions(source, ('input_calibration_valid', 'solar_os_input_pointer_calibration_default'))
        fixture = r'''
#include <assert.h>
#include <stdio.h>
#include "solar_os_input.h"
#define portENTER_CRITICAL(lock) ((void)(lock))
#define portEXIT_CRITICAL(lock) ((void)(lock))
static int input_lock;
static struct { uint32_t capabilities; } input_sources[2];
static struct { bool calibration_enabled; solar_os_input_pointer_calibration_t calibration; } input_diagnostics[2];
static bool input_source_valid_locked(solar_os_input_source_t source) { return source == 1; }
/* FUNCTIONS */
int main(void) {
    solar_os_input_pointer_calibration_t c = {0,4095,0,4095,480,320};
    assert(solar_os_input_pointer_calibration_default(0,&c) == ESP_ERR_INVALID_STATE);
    assert(solar_os_input_pointer_calibration_default(1,&c) == ESP_ERR_INVALID_STATE);
    input_sources[0].capabilities = SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE;
    assert(solar_os_input_pointer_calibration_default(1,&c) == ESP_OK);
    assert(input_diagnostics[0].calibration_enabled && input_diagnostics[0].calibration.width == 480);
    c.width = 320;
    assert(solar_os_input_pointer_calibration_default(1,&c) == ESP_OK);
    assert(input_diagnostics[0].calibration.width == 480);
    c.max_x = c.min_x;
    assert(solar_os_input_pointer_calibration_default(1,&c) == ESP_ERR_INVALID_ARG);
    puts("XPT2046 calibration: volatile defaults preserve configured calibration");
}
'''
        self.compile_run(fixture.replace('/* FUNCTIONS */', functions))


if __name__ == '__main__':
    unittest.main()
