from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class BQ27220Test(unittest.TestCase):
    def test_gauge_percentage_and_failed_reads(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory, "bq27220")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-include", str(ROOT / "tests/host/compat.h"),
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src"), "-idirafter", str(ROOT / "include"),
                "-I", str(ROOT / "tests/host/expansion_stubs"),
                str(ROOT / "tests/host/bq27220_test.c"),
                str(ROOT / "src/services/solar_os_bq27220.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
