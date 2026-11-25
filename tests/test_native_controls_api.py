from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class NativeControlsApiTest(unittest.TestCase):
    def test_real_parameters_and_controls_through_stable_records_and_claims(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'controls'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1', '-DSOLAR_OS_PACKAGE_SERVICE_CONTROLS=1',
                '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_controls_api_test.c'),
                *(str(ROOT / ('src/services/' + name)) for name in ['solar_os_parameters.c', 'solar_os_controls.c',
                    'solar_os_native_handles.c', 'solar_os_native_controls_api.c']), '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
