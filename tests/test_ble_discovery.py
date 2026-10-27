from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class BleDiscoveryTest(unittest.TestCase):
    def test_discovery_yields_keyboard_reconnect_and_releases_on_every_result(self):
        source = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text()
        functions = production_functions(source, (
            "solar_os_ble_keyboard_client_acquire",
            "solar_os_ble_keyboard_client_release",
            "solar_os_ble_backend_scan"))
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "ble_discovery_functions.inc").write_text(functions)
            binary = path / "test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + directory, "-I" + str(ROOT / "tests/host"),
                str(ROOT / "tests/host/ble_discovery_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
