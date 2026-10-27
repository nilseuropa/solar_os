from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class Sx1262Test(unittest.TestCase):
    def test_sleep_wakes_before_waiting_on_busy(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, "sx1262")
            subprocess.run([
                "cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I", str(ROOT / "tests/host/gpio_controller_stubs"),
                "-I", str(ROOT / "tests/host/inputronic_stubs"),
                "-I", str(ROOT / "tests/host/expansion_stubs"),
                "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src/drivers"),
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/sx1262_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
