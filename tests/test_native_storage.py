from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class NativeStorageTest(unittest.TestCase):
    def test_scopes_bounds_cancellation_and_recoverable_replacement(self):
        source = (ROOT / "src/services/solar_os_storage.c").read_text()
        replacements = production_functions(source, (
            "storage_errno_to_esp", "storage_metadata_from_stat", "storage_ensure_directory",
            "solar_os_storage_scandir", "solar_os_storage_makedirs", "solar_os_storage_rmdir",
            "solar_os_storage_sync_file", "solar_os_storage_sibling_path",
            "solar_os_storage_replace_file", "solar_os_storage_replace_files"))
        with tempfile.TemporaryDirectory() as temporary:
            Path(temporary, "storage_replace.inc").write_text(replacements)
            binary = Path(temporary, "storage")
            subprocess.run([
                "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-O1", "-g",
                "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                "-I", temporary, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "include"), "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/native_storage_test.c"),
                str(ROOT / "src/services/solar_os_native_storage.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary), temporary], check=True)


if __name__ == "__main__":
    unittest.main()
