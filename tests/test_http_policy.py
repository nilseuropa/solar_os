from pathlib import Path
import os
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]
SDK = Path(os.environ.get("IDF_PATH", str(Path.home() / ".platformio/packages/framework-espidf")))


class HttpPolicyTest(unittest.TestCase):
    @unittest.skipUnless((SDK / "components/http_parser/http_parser.c").exists(), "ESP-IDF parser unavailable")
    def test_production_redirect_drops_credentials_and_rewrites_methods(self):
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "http_follow_redirect.inc").write_text(production_functions(
                (ROOT / "src/services/solar_os_http_client.c").read_text(),
                ("solar_os_http_follow_redirect",)))
            binary = Path(directory, "redirect")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + directory, "-I" + str(ROOT / "tests/host"),
                "-I" + str(SDK / "components/http_parser"), "-I" + str(ROOT / "src/services"),
                str(ROOT / "tests/host/http_redirect_test.c"),
                str(ROOT / "src/services/solar_os_http_policy.c"),
                str(SDK / "components/http_parser/http_parser.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    @unittest.skipUnless((SDK / "components/http_parser/http_parser.c").exists(), "ESP-IDF parser unavailable")
    def test_redirect_resolution_and_origin_policy(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory, "policy")
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + str(SDK / "components/http_parser"),
                "-I" + str(ROOT / "src/services"),
                str(ROOT / "tests/host/http_policy_test.c"),
                str(ROOT / "src/services/solar_os_http_policy.c"),
                str(SDK / "components/http_parser/http_parser.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
