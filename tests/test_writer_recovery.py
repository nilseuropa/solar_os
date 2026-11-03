from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class WriterRecoveryTest(unittest.TestCase):
    def test_repeated_fatfs_snapshots_and_failed_replacement(self):
        source = (ROOT / "src/apps/solar_os_writer.c").read_text()
        names = ("writer_save_metadata", "writer_write_recovery")
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "writer_recovery.inc").write_text(production_functions(source, names))
            binary = Path(tmp, "writer-recovery")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-DSOLAR_OS_WRITER_HOST_TEST", "-Drename=writer_test_rename",
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/apps"), "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/writer_recovery_test.c"),
                str(ROOT / "src/apps/solar_os_writer_buffer.c"),
                str(ROOT / "src/apps/solar_os_writer_files.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
