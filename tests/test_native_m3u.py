from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeM3uTest(unittest.TestCase):
    def test_real_parser_callbacks_cancellation_file_lifetimes_and_write_rollback(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'm3u'
            subprocess.run(['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L',
                '-DSOLAR_OS_PACKAGE_SERVICE_M3U=1', '-Wall', '-Wextra', '-Werror',
                '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-include', str(ROOT / 'tests/host/compat.h'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_m3u_test.c'), str(ROOT / 'src/services/solar_os_m3u.c'),
                str(ROOT / 'src/services/solar_os_native_m3u.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_absent_service_keeps_backend_headers_private(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / 'absent.c'
            binary = Path(directory) / 'absent'
            source.write_text('#include <assert.h>\n#include "solar_os_native_m3u.h"\n'
                'int main(void){assert(!solar_os_native_m3u_get_service("media.m3u",1,8));return 0;}\n')
            subprocess.run(['cc', '-std=c11', '-DSOLAR_OS_PACKAGE_SERVICE_M3U=0',
                '-Wall', '-Wextra', '-Werror', '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src/services'),
                str(source), str(ROOT / 'src/services/solar_os_native_m3u.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
