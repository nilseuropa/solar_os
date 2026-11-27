from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class NativeDiagnosticsApiTest(unittest.TestCase):
    def test_concurrent_clipboard_snapshots_and_log_values(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'diagnostics'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pthread',
                '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-DSOLAR_OS_PACKAGE_CORE_RUNTIME=1',
                '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host'),
                '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/native_diagnostics_api_test.c'), str(ROOT / 'src/services/solar_os_clipboard.c'),
                str(ROOT / 'src/services/solar_os_native_diagnostics.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)
