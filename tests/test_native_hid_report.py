from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeHidReportTest(unittest.TestCase):
    def run_fixture(self, enabled):
        command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g', '-fsanitize=address,undefined',
                   '-fno-pie', '-no-pie', '-pthread', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=' + str(int(enabled))]
        for path in ('tests/host/native_abi_stubs', 'tests/host', 'include', 'src', 'src/services'):
            command += ['-I', str(ROOT / path)]
        sources = [ROOT / 'src/services/solar_os_native_hid_report.c']
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            if enabled:
                sources += [ROOT / 'tests/host/native_hid_report_test.c',
                            ROOT / 'src/services/solar_os_hid_keyboard_report.c',
                            ROOT / 'src/services/solar_os_native_handles.c',
                            ROOT / 'src/services/solar_os_hid_report_descriptor.c']
            else:
                source = stage / 'absent.c'
                source.write_text('#include <assert.h>\n#include "solar_os_native_hid_report.h"\n'
                    'int main(void){assert(!solar_os_native_hid_report_get_service("hid.report",1,8));return 0;}\n')
                sources.append(source)
            binary = stage / 'reports'
            subprocess.run(command + list(map(str, sources)) + ['-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_real_kernels_through_public_table(self):
        self.run_fixture(True)

    def test_absent_without_kernel_dependencies(self):
        self.run_fixture(False)

    def test_tracker_representation_stays_private(self):
        public = (ROOT / 'include/solar_os_native_hid_report_abi.h').read_text()
        public += (ROOT / 'include/solar_os_hid_types_v1.h').read_text()
        self.assertNotIn('solar_os_hid_keyboard_report_tracker_t', public)
        self.assertNotIn('solar_os_hid_keyboard_report_stream_t', public)
        self.assertNotIn('SOLAR_OS_HID_KEYBOARD_REPORT_STREAMS', public)
