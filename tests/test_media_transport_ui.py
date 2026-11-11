from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MediaTransportUiTest(unittest.TestCase):
    def test_production_clock_time_and_icon_geometry(self):
        video = (ROOT / "src/apps/solar_os_vplay.c").read_text()
        widgets = (ROOT / "src/services/solar_os_media_widgets.c").read_text()
        helpers = widgets[widgets.index("static void media_transport_triangle"):
                          widgets.index("esp_err_t solar_os_cassette_widget_create")]
        for name in ("paused", "clock_locked", "playback_status_text", "draw_play_time"):
            match = re.search(r"^static [^\n]*\b" + name + r"\([\s\S]*?^}", video, re.M)
            self.assertIsNotNone(match)
            helpers += "\n" + match.group()
        radio = (ROOT / "src/apps/solar_os_webradio.c").read_text()
        for name in ("webradio_progress_text", "webradio_player_samples", "webradio_should_pause", "webradio_toggle_pause"):
            match = re.search(r"^static [^\n]*\b" + name + r"\([\s\S]*?^}", radio, re.M)
            self.assertIsNotNone(match)
            helpers += "\n" + match.group()
        with tempfile.TemporaryDirectory(prefix="solaros-media-ui-") as directory:
            tmp = Path(directory)
            (tmp / "media_transport_ui.inc").write_text(helpers)
            binary = tmp / "media_transport_ui_test"
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                f"-I{tmp}", f"-I{ROOT / 'tests/host'}", f"-I{ROOT / 'src'}",
                f"-I{ROOT / 'src/services'}",
                str(ROOT / "tests/host/media_transport_ui_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_tick_updates_time_without_redrawing_video(self):
        video = (ROOT / "src/apps/solar_os_vplay.c").read_text()
        self.assertIn("else if (draw_play_time(p, gfx, display_time, false))", video)
        self.assertIn("if ((dirty || chrome) && p->current", video)
        self.assertIn("p->seeking ? (int64_t)(p->start_time * 1000000) : clock", video)

    def test_all_three_apps_use_common_chrome_and_hit_testing(self):
        for app in ("player", "webradio", "vplay"):
            source = (ROOT / f"src/apps/solar_os_{app}.c").read_text()
            for helper in ("layout", "header_draw", "controls_draw", "status_draw", "button_at"):
                self.assertIn("solar_os_media_player_" + helper, source)
            self.assertNotIn("height * 2 / 3", source)
            self.assertNotIn("(height * 2) / 3", source)
            self.assertIn("SOLAR_OS_APP_FLAG_POINTER_EVENTS", source)

    def test_webradio_graphical_enter_stops_and_space_pauses(self):
        radio = (ROOT / "src/apps/solar_os_webradio.c").read_text()
        controls = radio.split("static bool webradio_handle_graphics_key", 1)[1].split(
            "static bool webradio_event", 1)[0]
        self.assertIn("case '\\r':\n        case '\\n':\n            webradio_toggle_playback();", controls)
        self.assertIn("case ' ':\n            webradio_toggle_pause();", controls)
        self.assertIn(".should_pause = webradio_should_pause", radio)
        stop = radio.split("static void webradio_stop_playback", 1)[1].split(
            "static void webradio_reap_finished_task", 1)[0]
        self.assertIn("webradio.stop_requested = true", stop)
        self.assertIn("webradio.paused = false", stop)
        video = (ROOT / "src/apps/solar_os_vplay.c").read_text()
        self.assertNotIn("key == ' ' && p->stopped", video)
        self.assertIn("if (!p || p->stopped || p->done) return;", video)


if __name__ == "__main__":
    unittest.main()
