from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RadioScanWorkerTest(unittest.TestCase):
    def test_channel_controls_and_pending_frequency(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, "rfscan-controls")
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-no-pie",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-D_POSIX_C_SOURCE=200809L",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src/shell"),
                "-I", str(ROOT / "src"), "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/rfscan_controls_test.c"),
                str(ROOT / "src/services/solar_os_tui_widgets.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)

    def test_sampling_snapshot_and_shutdown_lifetime(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, "radio-scan-worker")
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-pthread", "-fsanitize=address,undefined", "-no-pie",
                "-D_POSIX_C_SOURCE=200809L", "-include", str(ROOT / "tests/host/compat.h"),
                "-I", str(ROOT / "tests/host/radio_scan_stubs"),
                "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/services"), "-I", str(ROOT / "src"),
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/radio_scan_worker_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=15)
