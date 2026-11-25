from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[1]

class NativeFsApiTest(unittest.TestCase):
    def test_filesystem_io_metadata_topology_and_cancellation(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary=Path(tmp)/'fs'
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-pthread',
                '-fsanitize=address,undefined','-fno-pie','-no-pie',
                '-I',str(ROOT/'tests/host'),'-I',str(ROOT/'include'),
                '-I',str(ROOT/'src'),'-I',str(ROOT/'src/services'),
                str(ROOT/'tests/host/native_fs_api_test.c'),
                str(ROOT/'src/services/solar_os_native_fs.c'),'-o',str(binary)],check=True)
            subprocess.run([str(binary),tmp],check=True,timeout=20)
