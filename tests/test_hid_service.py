from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class HidServiceTest(unittest.TestCase):
    def test_real_service_initialization_and_report_queue(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'hid'
            command = ['cc', '-std=c11', '-D_POSIX_C_SOURCE=200809L', '-Wall', '-Wextra', '-Werror',
                       '-g', '-O1', '-pthread', '-fsanitize=address,undefined', '-fno-pie', '-no-pie']
            for path in ('tests/host/hid_stubs', 'tests/host', 'include', 'src', 'src/drivers', 'src/services'):
                command += ['-I', str(ROOT / path)]
            subprocess.run(command + [str(ROOT / 'tests/host/hid_service_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
