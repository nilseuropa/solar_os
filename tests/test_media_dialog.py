from pathlib import Path
import subprocess
import tempfile
import unittest
import re
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class MediaDialogTest(unittest.TestCase):
    def test_webradio_terminal_input_draws_after_help(self):
        source = (ROOT / "src/apps/solar_os_webradio.c").read_text()
        render = source.split("static void webradio_render_tui(void)", 1)[1].split(
            "static void webradio_draw_graphics_header", 1)[0]
        self.assertLess(render.index("solar_os_tui_draw_help"), render.index("solar_os_tui_draw_input"))

    def test_shared_input_used_by_all_media_apps(self):
        for app in ("webradio", "rtsp_app", "recorder"):
            source = (ROOT / f"src/apps/solar_os_{app}.c").read_text()
            self.assertIn("ui_api->input_key", source)
            self.assertIn("ui_api->input_text", source)
            self.assertIn("SOLAR_OS_APP_FLAG_TEXT_EVENTS", source)
        with tempfile.TemporaryDirectory(prefix="solaros-media-dialog-") as directory:
            stage = Path(directory)
            radio = (ROOT / 'src/apps/solar_os_webradio.c').read_text()
            recorder = (ROOT / 'src/apps/solar_os_recorder.c').read_text()
            enum = re.search(r'typedef enum \{\s*WEBRADIO_DIALOG_NONE[\s\S]*?} webradio_dialog_t;', radio).group()
            constant = re.search(r'^#define RECORDER_FILENAME_MAX .*$', recorder, re.M).group()
            (stage / 'media_dialog_types.inc').write_text(enum + '\n' + constant)
            (stage / 'media_dialog_handlers.inc').write_text(production_functions(radio, (
                'webradio_dialog_set_input', 'webradio_cancel_dialog', 'webradio_dialog_input_capacity',
                'webradio_handle_dialog_text', 'webradio_handle_dialog_key')) + production_functions(recorder, (
                'recorder_begin_filename_edit', 'recorder_handle_filename_text', 'recorder_handle_filename_key')))
            binary = stage / "media_dialog_test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                "-fno-pie", "-no-pie", f"-I{stage}",
                f"-I{ROOT / 'tests/host'}", f"-I{ROOT / 'src'}", f"-I{ROOT / 'src/services'}", f"-I{ROOT / 'src/apps'}",
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/media_editor_apps_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
