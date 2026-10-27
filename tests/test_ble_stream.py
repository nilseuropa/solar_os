"""Native CoC backpressure and real stream/port ownership under sanitizers."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
HOST = ROOT / "tests/host"


class BleStreamTest(unittest.TestCase):
    def compile_and_run(self, sources):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "test"
            subprocess.run([
                "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra",
                "-Werror", "-Wno-unused-parameter", "-pthread",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-include", str(HOST / "compat.h"),
                "-I" + str(HOST / "nimble_stubs"), "-I" + str(HOST / "ble_stubs"),
                "-I" + str(HOST), "-I" + str(ROOT / "src/services"),
                "-idirafter", str(ROOT / "include"),
                *[str(ROOT / source) for source in sources], "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=15,
                           env={**os.environ, "UBSAN_OPTIONS": "halt_on_error=1"})

    def test_coc_flow_control_and_retirement(self):
        self.compile_and_run([
            "tests/host/ble_coc_test.c", "tests/host/nimble_test_support.c",
            "tests/host/ble_test_support.c", "src/services/solar_os_ble_hid_report_map.c",
        ])

    def test_registry_ownership_and_connection_failures(self):
        self.compile_and_run([
            "tests/host/ble_stream_test.c", "tests/host/ble_test_support.c",
            "src/services/solar_os_ble_stream.c", "src/services/solar_os_port.c",
            "src/services/solar_os_stream.c",
        ])
