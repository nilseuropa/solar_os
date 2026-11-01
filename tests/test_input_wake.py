from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InputWakeTest(unittest.TestCase):
    def test_events_wake_after_unlock_and_unchanged_samples_do_not(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory, 'input')
            subprocess.run(['cc', '-std=c11', '-D_GNU_SOURCE', '-Wall', '-Wextra', '-Werror',
                '-pthread', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-include', str(ROOT / 'tests/host/compat.h'),
                '-I' + str(ROOT / 'tests/host/registry_stubs'), '-I' + str(ROOT / 'tests/host'),
                '-I' + str(ROOT / 'src'), '-I' + str(ROOT / 'src/services'),
                '-idirafter', str(ROOT / 'include'), str(ROOT / 'tests/host/input_test.c'),
                str(ROOT / 'src/services/solar_os_input.c'), str(ROOT / 'src/services/solar_os_keys.c'),
                '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
