from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class MediaPreferencesTest(unittest.TestCase):
    def test_settings_reload_and_volume_isolation(self):
        with tempfile.TemporaryDirectory(prefix="solaros-media-preferences-") as directory:
            helpers = ""
            for app in ("player", "webradio"):
                source = (ROOT / f"src/apps/solar_os_{app}.c").read_text()
                helpers += production_functions(source, (
                    f"{app}_load_preferences", f"{app}_cycle_visualizer", f"{app}_adjust_volume")) + "\n"
                self.assertIn(f"{app}_load_preferences()", source.split(f"static esp_err_t {app}_start(", 1)[1])
            Path(directory, "media_player_preference_controls.inc").write_text(helpers)
            binary = Path(directory) / "preferences"
            subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-include", str(ROOT / "tests/host/compat.h"),
                f"-I{ROOT / 'tests/host/media_preferences_stubs'}", f"-I{ROOT / 'tests/host'}",
                f"-I{directory}",
                f"-I{ROOT / 'src'}", f"-I{ROOT / 'src/services'}", "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/media_preferences_test.c"),
                str(ROOT / "src/services/solar_os_media_preferences.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
