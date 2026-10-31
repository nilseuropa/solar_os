from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RegistryLifecycleTest(unittest.TestCase):
    def test_display_attachments_own_distinct_slots(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory, "display")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-pthread", "-include", str(ROOT / "tests/host/compat.h"),
                "-I" + str(ROOT / "tests/host/registry_stubs"),
                "-I" + str(ROOT / "tests/host/epd_stubs"),
                "-I" + str(ROOT / "tests/host/expansion_stubs"),
                "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src/board"),
                "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src"),
                "-I" + str(ROOT / "src/services"), "-I" + str(ROOT / "src/drivers"),
                "-I" + str(ROOT / "components/u8g2/src/clib"),
                str(ROOT / "tests/host/display_lifecycle_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_parameter_retirement_joins_callbacks_and_reserves_names(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory, "parameters")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I" + str(ROOT / "tests/host/registry_stubs"),
                "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src/services"),
                str(ROOT / "tests/host/parameter_lifecycle_test.c"),
                str(ROOT / "src/services/solar_os_parameters.c"), "-lm", "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

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
