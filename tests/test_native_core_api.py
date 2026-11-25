from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT=Path(__file__).resolve().parents[1]

class NativeCoreApiTest(unittest.TestCase):
    def test_handles_ports_crypto_and_clock_ownership(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'core'
            subprocess.run([
                'cc','-std=c11','-Wall','-Wextra','-Werror','-pthread',
                '-fsanitize=address,undefined','-fno-pie','-no-pie',
                '-I',str(ROOT/'tests/host/native_abi_stubs'),
                '-I',str(ROOT/'tests/host/model_bundle_stubs'),
                '-I',str(ROOT/'tests/host'),'-I',str(ROOT/'include'),
                '-I',str(ROOT/'src'),'-I',str(ROOT/'src/services'),
                str(ROOT/'tests/host/native_core_api_test.c'),
                str(ROOT/'src/services/solar_os_native_core.c'),
                '-lcrypto','-o',str(binary)],check=True)
            subprocess.run([str(binary)],check=True,timeout=20)
