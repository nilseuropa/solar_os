from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TuiLifecycleTest(unittest.TestCase):
    def test_failure_preservation_rollback_and_buffered_local_remote_frames(self):
        component = ROOT / 'components/u8g2'
        cmake = (component / 'CMakeLists.txt').read_text().split('\n)', 1)[0]
        sources = [component / name for name in re.findall(r'"([^"]+\.c)"', cmake)]
        sources += sorted((ROOT / 'fonts/build/u8g2').glob('u8g2_font_solar_os_default_*_tf.c'))
        sources += [ROOT / 'src/services' / name for name in ('solar_os_display_geometry.c',
            'solar_os_terminal_geometry.c', 'solar_os_terminal_preferences.c',
            'solar_os_terminal.c', 'solar_os_tui.c', 'solar_os_tui_widgets.c')]
        sources += [ROOT / 'src/services/solar_os_native_handles.c', ROOT / 'src/solar_os.c', ROOT / 'src/shell/solar_os_shell_io.c']
        sources += [ROOT / 'tests/host/application_delay.c']
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            compat = stage / 'compat.h'
            compat.write_text('#include <stddef.h>\nsize_t strlcpy(char *,const char *,size_t);\n'
                              'size_t strlcat(char *,const char *,size_t);\n')
            binary = stage / 'tui-lifecycle'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1',
                '-pthread', '-O1', '-g', '-ffunction-sections', '-fdata-sections', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-include', str(compat),
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'),
                '-I', str(ROOT / 'src/services'), '-I', str(ROOT / 'src/apps'), '-I', str(ROOT / 'src/shell'),
                '-I', str(component / 'src/clib'), str(ROOT / 'tests/host/tui_lifecycle_test.c'),
                *map(str, sources), '-Wl,--gc-sections', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
