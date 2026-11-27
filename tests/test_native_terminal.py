from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeTerminalTest(unittest.TestCase):
    def test_real_terminal_rendering_scrollback_values_and_preferences(self):
        component = ROOT / 'components/u8g2'
        cmake = (component / 'CMakeLists.txt').read_text().split('\n)', 1)[0]
        sources = [component / name for name in re.findall(r'"([^"]+\.c)"', cmake)]
        sources += sorted((ROOT / 'fonts/build/u8g2').glob('u8g2_font_solar_os_default_*_tf.c'))
        sources += [ROOT / 'src/services' / name for name in ('solar_os_display_geometry.c',
            'solar_os_terminal_geometry.c', 'solar_os_terminal_preferences.c',
            'solar_os_terminal.c', 'solar_os_native_terminal.c')]
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            compat = stage / 'compat.h'
            compat.write_text('#include <stddef.h>\nsize_t strlcpy(char *,const char *,size_t);\n'
                              'size_t strlcat(char *,const char *,size_t);\n')
            binary = stage / 'terminal'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1',
                '-O1', '-g', '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-include', str(compat),
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'),
                '-I', str(ROOT / 'src/services'), '-I', str(ROOT / 'src/apps'), '-I', str(ROOT / 'src/shell'),
                '-I', str(component / 'src/clib'), str(ROOT / 'tests/host/native_terminal_test.c'),
                *map(str, sources), '-Wl,--gc-sections', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_core_gate_and_public_opaque_reference(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'absent.c').write_text('#include <assert.h>\n#include "solar_os_native_terminal.h"\n'
                'int main(void){assert(!solar_os_native_terminal_get_service("terminal",1,8));return 0;}\n')
            subprocess.run(['cc', '-std=c11', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=0', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'),
                '-I', str(ROOT / 'src/services'), str(stage / 'absent.c'),
                str(ROOT / 'src/services/solar_os_native_terminal.c'), '-o', str(stage / 'absent')], check=True)
            subprocess.run([str(stage / 'absent')], check=True)
        public = (ROOT / 'include/solar_os_native_terminal_abi.h').read_text()
        self.assertNotIn('u8g2', public)
        self.assertNotIn('solar_os_context_t', public)
