from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get('IDF_PATH', str(Path.home() / '.platformio/packages/framework-espidf')))


class NativeHttpApiTest(unittest.TestCase):
    @unittest.skipUnless((SDK / 'components/http_parser/http_parser.c').exists(), 'ESP-IDF parser unavailable')
    def test_values_copied_requests_cancellation_and_retryable_shutdown(self):
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, 'http_copy_options.inc').write_text(production_functions(
                (ROOT / 'src/services/solar_os_http_stream.c').read_text(), ('http_stream_copy_options',)))
            binary = Path(tmp) / 'http'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                '-DSOLAR_OS_PACKAGE_SERVICE_HTTP_CLIENT=1', '-I', tmp,
                '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                '-I', str(SDK / 'components/http_parser'),
                str(ROOT / 'tests/host/native_http_api_test.c'),
                str(ROOT / 'src/services/solar_os_native_http.c'),
                str(ROOT / 'src/services/solar_os_native_handles.c'),
                str(ROOT / 'src/services/solar_os_http_policy.c'),
                str(SDK / 'components/http_parser/http_parser.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
