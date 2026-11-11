from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class VplayPlaylistTest(unittest.TestCase):
    def test_registry_accepts_bare_launch(self):
        registry = (ROOT / "src/apps/solar_os_app_registry.c").read_text()
        entry = registry.split('APP_FILE_ENTRY("vplay"', 1)[1].split("\n", 1)[0]
        self.assertIn(", 1, 3,", entry)

    def test_navigation_and_eof_wait_for_decoder_release(self):
        source = (ROOT / "src/apps/solar_os_vplay.c").read_text()
        state = source[source.index("typedef struct {\n    mpeg_player_t *player;"):
                       source.index("static esp_err_t vplay_start(")]
        functions = []
        functions.append(production_functions(source, (
            "vplay_start", "vplay_restart", "vplay_neighbor", "vplay_playlist_entry", "vplay_draw_playlist",
            "vplay_file_filter", "vplay_begin_browser", "vplay_activate_browser", "vplay_remove_selected",
            "vplay_transport", "vplay_event")))
        with tempfile.TemporaryDirectory(prefix="solaros-vplay-list-") as directory:
            tmp = Path(directory)
            (tmp / "vplay_playlist_state.inc").write_text(state)
            (tmp / "vplay_playlist_code.inc").write_text("\n".join(functions))
            binary = tmp / "vplay_playlist_test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                f"-I{tmp}", f"-I{ROOT / 'tests/host'}", f"-I{ROOT / 'src'}", f"-I{ROOT / 'src/services'}",
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/vplay_playlist_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
