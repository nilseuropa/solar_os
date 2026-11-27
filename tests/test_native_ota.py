from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeOtaTest(unittest.TestCase):
    def test_discovery_copied_values_and_shared_update_operations(self):
        services = ROOT / 'src/services'
        host = ROOT / 'tests/host'
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory, 'ota')
            for enabled in (0, 1):
                with self.subTest(ota_available=enabled):
                    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        f'-DSOLAR_OS_PACKAGE_SERVICE_OTA={enabled}',
                        '-I', str(host), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'),
                        '-I', str(services), str(host / 'native_ota_test.c'),
                        str(services / 'solar_os_native_device.c'), '-o', str(binary)], check=True)
                    subprocess.run([str(binary)], check=True, timeout=20)
