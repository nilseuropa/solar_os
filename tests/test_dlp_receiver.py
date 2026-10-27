"""Exercise the production receiver with the production DLP host under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class DlpReceiverTest(unittest.TestCase):
    def test_presentation_timing(self):
        source = (ROOT / "src/apps/solar_os_dlp_app.c").read_text()
        header = (ROOT / "src/apps/solar_os_dlp_app.h").read_text()
        stats = header[header.index("typedef struct"):header.index("} solar_os_dlp_presentation_stats_t;") +
            len("} solar_os_dlp_presentation_stats_t;")]
        with tempfile.TemporaryDirectory() as directory:
            Path(directory, "dlp_presentation_stats.inc").write_text(stats)
            Path(directory, "dlp_presentation_functions.inc").write_text(
                production_functions(source, ("draw_image", "present_image")))
            binary = Path(directory) / "timing"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + directory, "-I" + str(ROOT / "src/services"),
                "-I" + str(ROOT / "protocol/display_link/include"),
                str(ROOT / "tests/host/dlp_presentation_stats_test.c"),
                str(ROOT / "src/services/solar_os_dlp_raster.c"),
                str(ROOT / "protocol/display_link/src/sdlp.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_mono_raster(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "raster"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + str(ROOT / "src/services"),
                str(ROOT / "src/services/solar_os_dlp_raster.c"),
                str(ROOT / "tests/host/dlp_raster_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_receiver(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "receiver"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I" + str(ROOT / "src/services"),
                "-I" + str(ROOT / "protocol/display_link/include"),
                str(ROOT / "src/services/solar_os_dlp_receiver.c"),
                str(ROOT / "protocol/display_link/src/sdlp.c"),
                str(ROOT / "tests/host/dlp_receiver_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
