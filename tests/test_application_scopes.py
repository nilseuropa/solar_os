from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ApplicationScopesTest(unittest.TestCase):
    def test_real_lifecycle_nested_callbacks_threads_failures_and_cold_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'app-scopes'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-g', '-O1', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-include', str(ROOT / 'tests/host/compat.h'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                '-I', str(ROOT / 'src/shell'), '-I', str(ROOT / 'components/u8g2/src/clib'),
                str(ROOT / 'tests/host/application_scopes_test.c'), str(ROOT / 'src/solar_os.c'), str(ROOT / 'src/services/solar_os_native_handles.c'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
