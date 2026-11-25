from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeApplicationTest(unittest.TestCase):
    def test_real_context_lifecycle_requests_discovery_and_scope_ownership(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'application'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1',
                '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-O1', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                '-include', str(ROOT / 'tests/host/compat.h'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/apps'),
                '-I', str(ROOT / 'src/services'), '-I', str(ROOT / 'src/shell'),
                '-I', str(ROOT / 'components/u8g2/src/clib'),
                str(ROOT / 'tests/host/native_application_test.c'), str(ROOT / 'src/solar_os.c'),
                str(ROOT / 'src/services/solar_os_native_application.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_absent_core_and_public_boundary(self):
        with tempfile.TemporaryDirectory() as directory:
            source, binary = Path(directory) / 'absent.c', Path(directory) / 'absent'
            source.write_text('#include <assert.h>\n#include "solar_os_native_application.h"\n'
                'int main(void){assert(!solar_os_native_application_get_service("application",1,8));return 0;}\n')
            subprocess.run(['cc', '-std=c11', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=0', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'),
                '-I', str(ROOT / 'src/apps'), '-I', str(ROOT / 'src/services'), '-I', str(ROOT / 'src/shell'),
                str(source), str(ROOT / 'src/services/solar_os_native_application.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
        public = (ROOT / 'include/solar_os_native_application_abi.h').read_text()
        self.assertNotIn('solar_os_context_t', public)
        self.assertNotIn('solar_os_app_t', public)
