from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BleCallbackLifetimeTest(unittest.TestCase):
    def test_borrowed_peripheral_context_quiesces_before_close_and_is_never_reused(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'ble'
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                '-pthread', '-g', '-O1', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-include', str(ROOT / 'tests/host/compat.h'), '-I', str(ROOT / 'tests/host/ble_stubs'),
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/ble_callback_lifetime_test.c'), str(ROOT / 'tests/host/ble_test_support.c'),
                str(ROOT / 'src/services/solar_os_ble.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)
