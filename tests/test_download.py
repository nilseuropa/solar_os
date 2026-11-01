from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class DownloadTest(unittest.TestCase):
    def test_bounded_download_integrity_cancellation_and_failure_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory, "download.c")
            source.write_text((ROOT / "src/services/solar_os_download.c").read_text())
            binary = Path(directory, "download")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + str(ROOT / "tests/host/download_stubs"),
                "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src"),
                "-I" + str(ROOT / "src/services"), str(source),
                str(ROOT / "tests/host/download_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
