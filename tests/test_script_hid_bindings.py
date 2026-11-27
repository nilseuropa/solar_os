from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ScriptHidBindingsTest(unittest.TestCase):
    def run_runtime(self, language):
        command = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-DSOLAR_OS_PACKAGE_SERVICE_HID=1',
                   '-pthread', '-O1', '-g', '-include', str(ROOT / 'tests/host/compat.h')]
        for path in ('tests/host/hid_stubs', 'tests/host', 'include', 'src', 'src/drivers', 'src/services'):
            command += ['-I', str(ROOT / path)]
        sources = [ROOT / ('tests/host/hid_' + language + '_bindings_test.c'),
                   ROOT / 'src/services/solar_os_native_hid_device.c']
        if language == 'lua':
            runtime = ROOT / 'components/lua/lua/src'
            command += ['-I', str(runtime), '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
            sources += [p for p in runtime.glob('*.c') if p.name not in ('lua.c', 'luac.c')]
        else:
            component = ROOT / 'components/micropython_embed'
            runtime = component / 'micropython_embed'
            command += ['-I', str(component), '-I', str(runtime)]
            for directory in ('py', 'extmod', 'shared/runtime'):
                sources += list((runtime / directory).glob('*.c'))
            # Conservative VM stack scanning is intentional; the real Core
            # service/adapter and Lua fixture run under ASan/UBSan above.
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / language
            compiled = subprocess.run(command + list(map(str, sources)) + ['-lm', '-o', str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_real_lua_vm_and_hid_service(self):
        self.run_runtime('lua')

    def test_real_python_vm_and_hid_service(self):
        self.run_runtime('python')

    def test_binding_has_no_private_hid_calls_or_results(self):
        for language in ('lua', 'python'):
            source = (ROOT / ('src/apps/solar_os_' + language + '_hid.inc')).read_text()
            self.assertNotRegex(source, r'solar_os_hid_\w+\s*\(')
            self.assertNotIn('esp_err_t', source)
            self.assertNotIn('ESP_', source)
