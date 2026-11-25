from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class NativeBusApiTest(unittest.TestCase):
    def test_bus_values_protocol_leases_and_autobaud_cleanup(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'bus'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-DSOLAR_OS_PACKAGE_SERVICE_RESOURCES=1',
                '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host/expansion_stubs'),
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_bus_api_test.c'),
                str(ROOT / 'src/services/solar_os_native_handles.c'), str(ROOT / 'src/services/solar_os_native_bus.c'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
