"""Run the portable SDLP host/device interoperability and fault tests."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DisplayLinkTest(unittest.TestCase):
    def test_spi_exchange_and_deadlines(self):
        source = (ROOT / "src/drivers/solar_os_display_link_spi.c").read_text()
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "display_link_exchange.inc").write_text(
                production_functions(source, ("exchange",)))
            binary = Path(directory) / "exchange-test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + directory,
                str(ROOT / "tests/host/display_link_exchange_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_solaros_upload_and_conversion(self):
        library = ROOT / "protocol/display_link"
        source = (ROOT / "src/drivers/solar_os_display_link.c").read_text()
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "display_link_upload.inc").write_text(production_functions(
                source, ("pack_row", "upload", "raster_valid", "color", "native_to_logical")))
            binary = Path(directory) / "upload-test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I"+directory, "-I"+str(library / "include"),
                "-I"+str(ROOT / "src/services"),
                str(ROOT / "tests/host/display_link_upload_test.c"),
                str(library / "src/sdlp.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_bandwidth_and_streams(self):
        self.run_protocol_test("dlp_bandwidth_test.c")

    def test_portable_protocol(self):
        self.run_protocol_test("sdlp_test.c")

    def test_windowed_protocol(self):
        self.run_protocol_test("dlp_window_test.c")

    def run_protocol_test(self, name):
        library = ROOT / "protocol/display_link"
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "sdlp-test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + str(library / "include"), str(library / "src/sdlp.c"),
                str(library / "tests" / name), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
