from pathlib import Path
import subprocess
import tempfile
import unittest
from test_keyboard_layouts import write_keyboard_layouts

ROOT = Path(__file__).resolve().parents[1]


class NativeInputTest(unittest.TestCase):
    def test_real_sources_preferences_pressed_keys_transforms_and_composition(self):
        with tempfile.TemporaryDirectory() as directory:
            write_keyboard_layouts(directory)
            binary = Path(directory) / 'input'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1',
                '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-O1', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', directory, '-I', str(ROOT / 'tests/host/registry_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_input_test.c'), str(ROOT / 'src/services/solar_os_input.c'),
                str(ROOT / 'src/services/solar_os_keys.c'), str(ROOT / 'src/services/solar_os_input_composition.c'), str(ROOT / 'src/services/solar_os_native_input.c'),
                str(ROOT / 'src/services/solar_os_native_handles.c'), str(ROOT / 'tests/host/native_no_library.c'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_core_gate_and_value_only_public_records(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'absent.c').write_text('#include <assert.h>\n#include "solar_os_native_input.h"\n'
                'int main(void){assert(!solar_os_native_input_get_service("input",1,8));return 0;}\n')
            subprocess.run(['cc', '-std=c11', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=0', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(stage / 'absent.c'), str(ROOT / 'src/services/solar_os_native_input.c'), '-o', str(stage / 'absent')], check=True)
            subprocess.run([str(stage / 'absent')], check=True)
        public = (ROOT / 'include/solar_os_native_input_abi.h').read_text()
        self.assertNotIn('solar_os_context_t', public)
