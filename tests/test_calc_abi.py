from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class CalculatorAbiTest(unittest.TestCase):
    def test_command_text_and_graphics_with_real_services_and_lifecycle(self):
        component = ROOT / 'components/u8g2'
        cmake = (component / 'CMakeLists.txt').read_text().split('\n)', 1)[0]
        sources = [component / name for name in re.findall(r'"([^"]+\.c)"', cmake)]
        sources += sorted((ROOT / 'fonts/build/u8g2').glob('u8g2_font_solar_os_default_*_tf.c'))
        sources += [ROOT / 'src/services' / name for name in ('solar_os_display_geometry.c',
            'solar_os_gfx_icons.c', 'solar_os_native_gfx.c', 'solar_os_native_application.c', 'solar_os_expr.c')]
        sources += [ROOT / 'src/services/solar_os_native_handles.c', ROOT / 'src/solar_os.c', ROOT / 'src/apps/solar_os_calc.c']
        sources += [ROOT / 'tests/host/application_delay.c', ROOT / 'src/apps/solar_os_app_file_types.c']
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            decoder = stage / 'utf8.c'
            decoder.write_text('#include <stdint.h>\n#include <stddef.h>\n' + production_functions(
                (ROOT / 'src/services/solar_os_fonts.c').read_text(), ('solar_os_utf8_next',)))
            shell_fallback = stage / 'shell_fallback.c'
            shell_fallback.write_text('#include "solar_os.h"\n#include "solar_os_shell_io.h"\n'
                + production_functions((ROOT / 'src/shell/solar_os_shell_io.c').read_text(),
                    ('solar_os_shell_io_with_fallback',)))
            compat = stage / 'compat.h'
            compat.write_text('#include <stddef.h>\nsize_t strlcpy(char *,const char *,size_t);\n'
                              'size_t strlcat(char *,const char *,size_t);\n')
            binary = stage / 'calc'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1',
                '-O1', '-g', '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-include', str(compat),
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'),
                '-I', str(ROOT / 'src/services'), '-I', str(ROOT / 'src/apps'), '-I', str(ROOT / 'src/shell'),
                '-I', str(component / 'src/clib'), str(ROOT / 'tests/host/calc_abi_test.c'),
                str(decoder), str(shell_fallback), *map(str, sources), '-Wl,--gc-sections', '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
