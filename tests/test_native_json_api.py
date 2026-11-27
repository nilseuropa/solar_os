from pathlib import Path
import subprocess
import tempfile
import unittest
from host_cjson import CJSON
ROOT = Path(__file__).resolve().parents[1]


class NativeJsonApiTest(unittest.TestCase):
    @unittest.skipUnless((CJSON / 'cJSON.c').exists(), 'cJSON source unavailable')
    def test_full_os_json_and_legacy_prefix_with_real_parser_and_scanner(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'json'
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-fsanitize=address,undefined,float-cast-overflow', '-fno-pie', '-no-pie',
                '-include', str(ROOT / 'tests/host/compat.h'),
                '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                '-I', str(CJSON), str(ROOT / 'tests/host/native_json_api_test.c'),
                *(str(ROOT / ('src/services/' + name)) for name in ['solar_os_json.c', 'solar_os_json_scan.c', 'solar_os_native_json.c']),
                str(CJSON / 'cJSON.c'), '-lm', '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
