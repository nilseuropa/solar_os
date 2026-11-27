from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeBleClientTest(unittest.TestCase):
    def test_real_core_sessions_peer_isolation_values_oom_and_cancellation(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'ble'
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DSOLAR_OS_PACKAGE_SERVICE_BLE=1',
                '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-O1', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', str(ROOT / 'tests/host/ble_stubs'), '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'),
                '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_ble_client_test.c'), str(ROOT / 'tests/host/ble_test_support.c'),
                str(ROOT / 'src/services/solar_os_native_ble.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_unavailable_service_has_no_backend_dependency(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            source = stage / 'absent.c'
            source.write_text('#include <assert.h>\n#include "solar_os_native_ble.h"\n'
                'int main(void){assert(!solar_os_native_ble_get_service("ble.client",1,8));'
                'assert(!solar_os_native_ble_get_service("ble",1,8));return 0;}\n')
            subprocess.run(['cc', '-std=c11', '-DSOLAR_OS_PACKAGE_SERVICE_BLE=0', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(source), str(ROOT / 'src/services/solar_os_native_ble.c'), '-o', str(stage / 'absent')], check=True)
            subprocess.run([str(stage / 'absent')], check=True)
