from pathlib import Path
import subprocess
import tempfile
import unittest
from test_keyboard_layouts import write_keyboard_layouts

ROOT = Path(__file__).resolve().parents[1]


class NativeInputProducersTest(unittest.TestCase):
    def test_actual_producers_typed_handles_pool_rollback_and_application_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            write_keyboard_layouts(directory)
            binary = Path(directory) / 'input'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1',
                '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-O1', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', directory, '-I', str(ROOT / 'tests/host/registry_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                '-I', str(ROOT / 'src/shell'), '-I', str(ROOT / 'components/u8g2/src/clib'),
                str(ROOT / 'tests/host/native_input_producers_test.c'), str(ROOT / 'src/services/solar_os_input.c'),
                str(ROOT / 'src/services/solar_os_keys.c'), str(ROOT / 'src/services/solar_os_input_composition.c'), str(ROOT / 'src/services/solar_os_native_input.c'),
                str(ROOT / 'src/services/solar_os_native_handles.c'), str(ROOT / 'tests/host/native_no_library.c'), str(ROOT / 'src/solar_os.c'),
                str(ROOT / 'tests/host/application_delay.c'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
