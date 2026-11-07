from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class WifiLatencyTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "requires C compiler")
    def test_production_owner_lifecycle(self):
        source = (ROOT / "src/services/solar_os_wifi.c").read_text()
        header = (ROOT / "src/services/solar_os_wifi.h").read_text()
        constants = "\n".join(re.findall(r"^#define SOLAR_OS_WIFI_LATENCY_.*$", header, re.M))
        state = "\n".join(re.findall(r"^static .*wifi_latency_(?:owners|count).*;$", source, re.M))
        policy = source[source.index("static wifi_ps_type_t wifi_power_save_mode_locked("):
                        source.index("static wifi_ps_type_t wifi_power_save_mode(void)")]
        lifecycle = source[source.index("esp_err_t solar_os_wifi_latency_acquire("):
                           source.index("void solar_os_wifi_get_status(")]
        with tempfile.TemporaryDirectory(prefix="solaros-wifi-latency-") as directory:
            tmp = Path(directory)
            (tmp / "wifi_latency_production.h").write_text(constants + "\n" + state + "\n" + policy + lifecycle)
            binary = tmp / "wifi_latency_test"
            subprocess.run([
                "cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                f"-I{tmp}", str(ROOT / "tests/host/wifi_latency_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)
