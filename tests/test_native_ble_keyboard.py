from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeBleKeyboardTest(unittest.TestCase):
    def run_fixture(self, enabled):
        command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                   '-fno-pie', '-no-pie', '-DSOLAR_OS_PACKAGE_SERVICE_BLE=' + str(int(enabled))]
        for path in ('tests/host', 'include', 'src', 'src/services'):
            command += ['-I', str(ROOT / path)]
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            if enabled:
                source = ROOT / 'tests/host/native_ble_keyboard_test.c'
            else:
                source = stage / 'absent.c'
                source.write_text('#include <assert.h>\n#include "solar_os_native_ble_keyboard.h"\n'
                    'int main(void){assert(!solar_os_native_ble_keyboard_get_service("ble.keyboard",1,8));return 0;}\n')
            binary = stage / 'keyboard'
            subprocess.run(command + [str(source), str(ROOT / 'src/services/solar_os_native_ble_keyboard.c'),
                                     '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_normalized_records_and_results(self):
        self.run_fixture(True)

    def test_absent_without_hardware_backend(self):
        self.run_fixture(False)
