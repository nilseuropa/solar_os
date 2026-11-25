from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[1]


class NativeStreamApiTest(unittest.TestCase):
    def compile(self, fixture, sources, binary):
        subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror', '-pthread',
            '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-DSOLAR_OS_PACKAGE_SERVICE_STREAMS=1',
            '-include', str(ROOT / 'tests/host/compat.h'),
            '-I', str(ROOT / 'tests/host/native_abi_stubs'), '-I', str(ROOT / 'tests/host'),
            '-I', str(ROOT / 'include'), '-I', str(ROOT / 'src'), '-I', str(ROOT / 'src/services'),
            str(ROOT / ('tests/host/' + fixture)),
            *(str(ROOT / ('src/services/' + source)) for source in sources), '-lm', '-o', str(binary)], check=True)

    def test_native_values_and_frame_lifetime_with_the_real_stream_registry(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'native'
            self.compile('native_stream_api_test.c', ['solar_os_stream.c', 'solar_os_native_handles.c', 'solar_os_native_stream.c'], binary)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_existing_registry_behavior_and_typed_open(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'registry'
            self.compile('stream_test.c', ['solar_os_stream.c'], binary)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_port_close_refusal_preserves_claims_until_retry(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / 'port-stream'
            self.compile('native_port_stream_test.c', ['solar_os_stream.c', 'solar_os_port.c',
                'solar_os_native_handles.c', 'solar_os_native_stream.c'], binary)
            subprocess.run([str(binary)], check=True, timeout=20)
