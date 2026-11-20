from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]


class SyntaxTest(unittest.TestCase):
    def test_tokens_multiline_state_and_clipped_editor_buffers(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'syntax_test'
            subprocess.run([
                'cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-g',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                '-I', str(ROOT / 'src/services'),
                str(ROOT / 'tests/host/syntax_test.c'),
                str(ROOT / 'src/services/solar_os_syntax.c'),
                '-o', str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()
