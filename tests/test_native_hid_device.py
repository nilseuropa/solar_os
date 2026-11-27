from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeHidDeviceTest(unittest.TestCase):
    def run_fixture(self, enabled):
        command = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                   '-g', '-O1', '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                   '-DSOLAR_OS_PACKAGE_SERVICE_HID=' + str(int(enabled))]
        for path in ('tests/host/hid_stubs', 'tests/host', 'include', 'src', 'src/drivers', 'src/services'):
            command += ['-I', str(ROOT / path)]
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'hid'
            if enabled:
                source = ROOT / 'tests/host/native_hid_device_test.c'
            else:
                source = Path(directory) / 'absent.c'
                source.write_text('#include <assert.h>\n#include "solar_os_native_hid_device.h"\n'
                    'int main(void){assert(!solar_os_native_hid_device_get_service("hid.device",1,8));return 0;}\n')
            subprocess.run(command + [str(source), str(ROOT / 'src/services/solar_os_native_hid_device.c'),
                                     '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_real_service_and_normalized_controls(self):
        self.run_fixture(True)

    def test_absent_without_backend_dependencies(self):
        self.run_fixture(False)
