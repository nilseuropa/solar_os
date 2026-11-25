from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeBleStreamTest(unittest.TestCase):
    def run_fixture(self, enabled):
        command = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DSOLAR_OS_PACKAGE_SERVICE_BLE_STREAM=' + str(int(enabled)),
                   '-Wall', '-Wextra', '-Werror', '-pthread', '-O1', '-g', '-fsanitize=address,undefined',
                   '-fno-pie', '-no-pie', '-include', str(ROOT / 'tests/host/compat.h')]
        for path in ('tests/host/ble_stubs', 'tests/host', 'include', 'src', 'src/services'):
            command += ['-I', str(ROOT / path)]
        sources = [ROOT / 'src/services/solar_os_native_ble_stream.c']
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            if enabled:
                sources += [ROOT / 'tests/host/native_ble_stream_test.c', ROOT / 'tests/host/ble_test_support.c']
                sources += [ROOT / 'src/services' / name for name in
                            ('solar_os_ble_stream.c', 'solar_os_port.c', 'solar_os_stream.c')]
            else:
                source = stage / 'absent.c'
                source.write_text('#include <assert.h>\n#include "solar_os_native_ble_stream.h"\n'
                    'int main(void){assert(!solar_os_native_ble_stream_get_service("stream.ble",1,8));return 0;}\n')
                sources.append(source)
            binary = stage / 'streams'
            subprocess.run(command + list(map(str, sources)) + ['-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)

    def test_real_registry_port_and_stream_ownership(self):
        self.run_fixture(True)

    def test_absent_without_core_backend_dependencies(self):
        self.run_fixture(False)
