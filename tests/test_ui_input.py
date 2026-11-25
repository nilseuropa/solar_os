"""Renderer-independent input editing uses only the portable SDK."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class UiInputTest(unittest.TestCase):
    def test_shared_editor_and_utf8_navigation(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / 'input'
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic',
                '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-pie', '-no-pie',
                '-I', str(ROOT / 'include'), str(ROOT / 'tests/host/ui_input_test.c'), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
