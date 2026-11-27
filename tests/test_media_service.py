from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")


class MediaServicePolicyTest(unittest.TestCase):
    def test_media_package_contains_protocol_core(self):
        package = PACKAGES.split("[packages.service_media]", 1)[1].split(
            "[packages.", 1
        )[0]
        self.assertIn('label = "service.media"', package)
        self.assertIn('"services/solar_os_media.c"', package)
        self.assertIn('"services/solar_os_rtp.c"', package)
        self.assertIn('"services/solar_os_rtp_jpeg.c"', package)
        self.assertIn('"services/solar_os_rtsp.c"', package)

    def test_native_rtp_and_rtsp_use_the_production_protocol_code(self):
        host = ROOT / "tests/host"
        services = ROOT / "src/services"
        include = [f"-I{directory}" for directory in (host, ROOT / "include", ROOT / "src", services)]
        flags = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *include]
        adapter = str(services / "solar_os_native_media_protocol.c")
        subprocess.run([*flags, "-DSOLAR_OS_PACKAGE_SERVICE_MEDIA=0", "-fsyntax-only", adapter], check=True)
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp) / "protocol"
            sources = ["solar_os_media.c", "solar_os_rtp.c", "solar_os_rtp_jpeg.c", "solar_os_rtsp.c",
                       "solar_os_native_handles.c"]
            subprocess.run([*flags, "-DTEST_NATIVE_MEDIA_PROTOCOL", "-DSOLAR_OS_PACKAGE_SERVICE_MEDIA=1",
                "-fsanitize=address,undefined", "-no-pie", adapter, *(str(services / s) for s in sources),
                str(host / "media_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

if __name__ == "__main__":
    unittest.main()
