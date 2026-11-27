from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeExprTest(unittest.TestCase):
    def test_real_parser_callbacks_threads_and_typed_program_lifetimes(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'expr'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-DSOLAR_OS_PACKAGE_SERVICE_EXPR=1',
                '-Wall', '-Wextra', '-Werror', '-pthread', '-g', '-O1', '-fsanitize=address,undefined',
                '-fno-pie', '-no-pie', '-I', str(ROOT / 'tests/host/native_abi_stubs'),
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'),
                '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_expr_test.c'), str(ROOT / 'src/services/solar_os_expr.c'),
                str(ROOT / 'src/services/solar_os_native_expr.c'), str(ROOT / 'src/services/solar_os_native_handles.c'),
                '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_absent_backend_and_private_program_representation(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / 'absent.c').write_text('#include <assert.h>\n#include "solar_os_native_expr.h"\n'
                'int main(void){assert(!solar_os_native_expr_get_service("expr",1,8));return 0;}\n')
            subprocess.run(['cc', '-std=c11', '-DSOLAR_OS_PACKAGE_SERVICE_EXPR=0', '-Wall', '-Wextra', '-Werror',
                '-I', str(ROOT / 'tests/host'), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'),
                '-I', str(ROOT / 'src/services'), str(stage / 'absent.c'),
                str(ROOT / 'src/services/solar_os_native_expr.c'), '-o', str(stage / 'absent')], check=True)
            subprocess.run([str(stage / 'absent')], check=True)
        public = (ROOT / 'include/solar_os_native_expr_abi.h').read_text()
        self.assertNotIn('solar_os_expr_program_t', public)
        self.assertNotIn('solar_os_expr_node_t', public)
