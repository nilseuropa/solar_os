from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeSynthTest(unittest.TestCase):
    def test_leases_follow_renderer_and_voice_shutdown(self):
        host = ROOT / 'tests/host'
        services = ROOT / 'src/services'
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory, 'synth')
            for enabled in (0, 1):
                with self.subTest(synth_available=enabled):
                    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                        '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie',
                        f'-DSOLAR_OS_PACKAGE_SERVICE_SYNTH={enabled}',
                        '-DSOLAR_OS_PACKAGE_SERVICE_EXPANSION=1', '-I', str(host / 'native_abi_stubs'),
                        '-I', str(host), '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'),
                        '-I', str(services), str(host / 'native_synth_test.c'),
                        str(services / 'solar_os_native_synth.c'), str(services / 'solar_os_native_handles.c'),
                        '-o', str(binary)], check=True)
                    subprocess.run([str(binary)], check=True, timeout=20)
