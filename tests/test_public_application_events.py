"""Core and portable applications use one event value representation."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PublicApplicationEventsTest(unittest.TestCase):
    def test_core_callbacks_accept_public_events_and_opaque_context_without_adapters(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            source = stage / 'core.c'
            source.write_text('''#include <assert.h>
#include "solar_os.h"
_Static_assert(__builtin_types_compatible_p(solar_os_context_t, solar_os_application_context_t), "context identity");
_Static_assert(__builtin_types_compatible_p(solar_os_event_t, solar_os_application_event_v1_t), "event identity");
_Static_assert(__builtin_types_compatible_p(solar_os_event_type_t, solar_os_application_event_type_v1_t), "tag identity");
_Static_assert(__builtin_types_compatible_p(solar_os_app_t, solar_os_application_lifecycle_api_v1_t), "callback table identity");
static solar_os_context_t *expected_context;
static const solar_os_application_event_v1_t *expected_event;
static int start(solar_os_application_context_t *context) { assert(context == expected_context); return 0; }
static bool event(solar_os_application_context_t *context, const solar_os_application_event_v1_t *value)
{ assert(context == expected_context && value == expected_event); return value->type == SOLAR_OS_EVENT_TEXT && value->data.text[0] == 'x'; }
int main(void) {
 const solar_os_app_t app = {.name="public-values", .start=start, .event=event};
 solar_os_context_t context = {0};
 solar_os_application_event_v1_t value = {.type=SOLAR_OS_EVENT_TEXT, .data.text="x"};
 expected_context=&context; expected_event=&value;
 assert(SOLAR_OS_EVENT_CHAR==0 && SOLAR_OS_EVENT_KEY==1 && SOLAR_OS_EVENT_POINTER==2 && SOLAR_OS_EVENT_AXIS==3);
 assert(SOLAR_OS_EVENT_GESTURE==4 && SOLAR_OS_EVENT_TICK==5 && SOLAR_OS_EVENT_RESUME==6 && SOLAR_OS_EVENT_TEXT==7);
 assert(!app.start(&context) && app.event(&context, &value)); return 0;
}
''')
            binary = stage / 'core'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'),
                '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_isolated_c_and_cpp_sdk_and_private_context(self):
        import sys
        sys.path.insert(0, str(ROOT / 'scripts'))
        from generate_native_api import load
        headers, _ = load(ROOT)
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            for name, content in headers.items():
                (stage / name).write_text(content)
            source = stage / 'event.c'
            source.write_text('''#include <stddef.h>
#include <string.h>
#include "solar_os_native_application_events_abi.h"
#include "solar_os_native_application_lifecycle_abi.h"
static bool read_event(solar_os_application_context_t *context, const solar_os_application_event_v1_t *event)
{ (void)context; return event->type == SOLAR_OS_EVENT_TEXT && event->data.text[0] == 'x'; }
int main(void) {
 solar_os_application_event_v1_t event = {SOLAR_OS_EVENT_TEXT, {0}};
 solar_os_application_lifecycle_api_v1_t callbacks;
 memset(&callbacks,0,sizeof(callbacks));
 callbacks.abi_version=SOLAR_OS_APPLICATION_LIFECYCLE_ABI;
 callbacks.struct_size=sizeof(callbacks); callbacks.event=read_event;
 event.data.text[0]='x';
 return callbacks.struct_size>=SOLAR_OS_APPLICATION_LIFECYCLE_V1_MIN_SIZE && callbacks.event(NULL,&event) ? 0 : 1;
}
''')
            for compiler, standard in [('cc', 'c11'), ('c++', 'c++17')]:
                binary = stage / compiler.replace('+', 'p')
                subprocess.run([compiler, '-std=' + standard, '-Wall', '-Wextra', '-Werror',
                    '-pedantic', '-I', str(stage), str(source), '-o', str(binary)], check=True)
                subprocess.run([str(binary)], check=True)
            private = stage / 'private.c'
            private.write_text('#include "solar_os_native_application_events_abi.h"\n'
                'int field(solar_os_application_context_t *context){return context->argc;}\n')
            result = subprocess.run(['cc', '-std=c11', '-Werror', '-I', str(stage),
                '-c', str(private), '-o', str(stage / 'private.o')], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('incomplete', result.stderr)

    @unittest.skipUnless((ROOT / '.pio/tinycc/solar-tcc').is_file(), 'OS C frontend required')
    def test_os_c_frontend_emits_a_loader_valid_public_event_module(self):
        import json
        from test_external_library_sdk import builtin_headers
        cache = ROOT / '.pio/tinycc'
        pin = json.loads((ROOT / 'modules/experimental/tinycc/upstream.json').read_text())['revision']
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            sdk = stage / 'sdk'; sdk.mkdir()
            for name, path in builtin_headers(cache / 'source' / ('tinycc-' + pin)).items():
                shutil.copyfile(path, sdk / name)
            source, obj, artifact = stage / 'event.c', stage / 'event.o', stage / 'event.elf'
            source.write_text('#include "solar_os_native_abi.h"\n'
                '#include "solar_os_native_application_events_abi.h"\n'
                '#include "solar_os_native_application_lifecycle_abi.h"\n'
                'int main(int argc,char **argv){(void)argc;(void)argv;'
                'solar_os_application_event_v1_t event={SOLAR_OS_EVENT_TEXT,{0}};'
                'event.data.text[0]=\'x\';return solar_os_native_host_v1()->write_utf8(event.data.text,1);}\n')
            subprocess.run([str(cache / 'solar-tcc'), str(source), str(obj), str(sdk)], check=True)
            subprocess.run([str(cache / 'module'), str(obj), str(artifact), 'main'], check=True)
            subprocess.run([str(cache / 'elf-check'), str(artifact)], check=True)
