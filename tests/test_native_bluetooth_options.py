from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeBluetoothOptionsTest(unittest.TestCase):
    def test_borrowed_callbacks_parallel_peers_cleanup_and_peripheral_options(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'options'
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DSOLAR_OS_PACKAGE_SERVICE_BLE=1',
                '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-O1', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', str(ROOT / 'tests/host/ble_stubs'), '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'),
                '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_bluetooth_options_test.c'), str(ROOT / 'tests/host/ble_test_support.c'),
                str(ROOT / 'src/services/solar_os_native_ble.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)
