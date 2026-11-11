from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MediaDialogTest(unittest.TestCase):
    def test_webradio_terminal_input_draws_after_help(self):
        source = (ROOT / "src/apps/solar_os_webradio.c").read_text()
        render = source.split("static void webradio_render_tui(void)", 1)[1].split(
            "static void webradio_draw_graphics_header", 1)[0]
        self.assertLess(render.index("solar_os_tui_draw_help"), render.index("solar_os_tui_move"))
        self.assertLess(render.index("solar_os_tui_draw_help"),
                        render.index("solar_os_tui_set_cursor_visible(&webradio.tui, true)"))

    def test_shared_input_used_by_radio_and_rtsp(self):
        for app in ("webradio", "rtsp_app"):
            self.assertIn("solar_os_media_dialog_input_key", (ROOT / f"src/apps/solar_os_{app}.c").read_text())
        with tempfile.TemporaryDirectory(prefix="solaros-media-dialog-") as directory:
            binary = Path(directory) / "media_dialog_test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                f"-I{ROOT / 'tests/host'}", f"-I{ROOT / 'src'}", f"-I{ROOT / 'src/services'}",
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/media_dialog_test.c"),
                str(ROOT / "src/services/solar_os_media_dialog.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
