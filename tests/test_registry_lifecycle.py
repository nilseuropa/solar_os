from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RegistryLifecycleTest(unittest.TestCase):
    def test_concurrent_first_mutex_access(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory, "sync")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-pthread", "-I" + str(ROOT / "tests/host/registry_stubs"),
                "-I" + str(ROOT / "src/services"),
                str(ROOT / "tests/host/sync_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
