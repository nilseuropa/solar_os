"""Execute package-owned Bluetooth bindings with the real VM and Core ABI."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
import test_tcp_stream as tcp_support

ROOT = Path(__file__).resolve().parents[1]


class ScriptBleBindingsTest(unittest.TestCase):
    def run_runtime(self, language):
        runtime = ROOT / 'components/lua/lua/src'
        sources = [ROOT / ('tests/host/ble_' + language + '_bindings_test.c'), ROOT / 'tests/host/ble_test_support.c',
                   ROOT / 'src/services/solar_os_ble.c', ROOT / 'src/services/solar_os_native_ble.c',
                   ROOT / 'src/services/solar_os_native_ble_keyboard.c',
                   *[ROOT / 'src/services' / name for name in ('solar_os_native_ble_stream.c',
                       'solar_os_ble_stream.c','solar_os_port.c','solar_os_stream.c',
                       'solar_os_tcp_stream.c','solar_os_native_tcp_stream.c')]]
        command = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DSOLAR_OS_PACKAGE_SERVICE_BLE=1',
                   '-DSOLAR_OS_PACKAGE_SERVICE_BLE_STREAM=1',
                   '-DSOLAR_OS_PACKAGE_SERVICE_TCP_STREAM=1',
                   '-pthread', '-O1', '-g', '-include', str(ROOT / 'tests/host/compat.h')]
        for directory in ('tests/host/ble_stubs', 'tests/host', 'include', 'src', 'src/services', 'src/apps'):
            command += ['-I', str(ROOT / directory)]
        if language == 'lua':
            sources += [p for p in runtime.glob('*.c') if p.name not in ('lua.c', 'luac.c')]
            command += ['-I', str(runtime), '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
        else:
            component = ROOT / 'components/micropython_embed'
            runtime = component / 'micropython_embed'
            command += ['-I', str(component), '-I', str(runtime)]
            for directory in ('py', 'extmod', 'shared/runtime'):
                sources += list((runtime / directory).glob('*.c'))
            # MicroPython's conservative stack GC intentionally reads C stack
            # padding. The service and adapter are covered with ASan above.
        with tempfile.TemporaryDirectory() as directory:
            tcp_support.tcp_headers(Path(directory), with_semaphores=False)
            command += ['-I', directory]
            binary = Path(directory) / language
            compiled = subprocess.run(command + list(map(str, sources)) + ['-lm', '-o', str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_lua_against_real_service_and_interpreter(self):
        self.run_runtime('lua')

    def test_python_against_real_service_and_vm(self):
        self.run_runtime('python')

    def test_bindings_use_only_public_bluetooth_contract(self):
        for language in ('lua', 'python'):
            source = (ROOT / ('src/apps/solar_os_' + language + '_ble.inc')).read_text()
            self.assertNotRegex(source, r'solar_os_ble_\w+\s*\(')
            self.assertNotIn('esp_err_t', source)
            self.assertNotIn('ESP_', source)
            self.assertNotIn('set_cancel_check', source)
            stream = (ROOT / ('src/apps/solar_os_' + language + '_ble_stream.inc')).read_text()
            self.assertNotRegex(stream, r'solar_os_ble_stream_\w+\s*\(')
            self.assertNotIn('ESP_', stream)
            tcp = (ROOT / ('src/apps/solar_os_' + language + '_tcp_stream.inc')).read_text()
            self.assertNotRegex(tcp, r'solar_os_tcp_stream_\w+\s*\(')
            self.assertNotIn('ESP_', tcp)


if __name__ == '__main__':
    unittest.main()
