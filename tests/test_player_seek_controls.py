from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class PlayerSeekControlsTest(unittest.TestCase):
    def test_production_seek_target_and_pause_logic(self):
        bodies = []
        for app in ("player", "vplay"):
            source = (ROOT / f"src/apps/solar_os_{app}.c").read_text()
            body = re.search(r"^static void " + app + r"_seek\([\s\S]*?^}", source, re.M)
            self.assertIsNotNone(body)
            bodies.append(body.group())
            if app == "player":
                for name in ("player_progress_callback", "player_device_callback", "player_toggle_pause", "player_state_symbol"):
                    body = re.search(r"^static [^\n]*\b" + name + r"\([\s\S]*?^}", source, re.M)
                    self.assertIsNotNone(body)
                    bodies.append(body.group())
        with tempfile.TemporaryDirectory(prefix="solaros-player-seek-") as directory:
            tmp = Path(directory)
            (tmp / "player_seek_controls.inc").write_text("\n".join(bodies))
            binary = tmp / "player_seek_controls_test"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                f"-I{tmp}", f"-I{ROOT / 'tests/host'}",
                f"-I{ROOT / 'src/services'}", f"-I{ROOT / 'src'}",
                str(ROOT / "tests/host/player_seek_controls_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_keyboard_pointer_controls_and_bounded_lifecycle(self):
        player = (ROOT / "src/apps/solar_os_player.c").read_text()
        video = (ROOT / "src/apps/solar_os_vplay.c").read_text()
        for source in (player, video):
            self.assertIn("key == '<' || key == '>'", source)
            self.assertIn("_seek(key == '<' ? -1 : 1)", source)
            self.assertNotIn("key == '[' || key == ']'", source)
            self.assertIn("solar_os_media_player_controls_draw", source)
            self.assertIn("solar_os_media_player_button_at", source)
            self.assertIn("SOLAR_OS_APP_FLAG_POINTER_EVENTS", source)
            self.assertIn("SOLAR_OS_EVENT_POINTER", source)
        self.assertIn("</> seek", player)
        self.assertIn("{SOLAR_OS_KEY_LEFT, '<', '\\r', '>', SOLAR_OS_KEY_RIGHT}", video)
        start = player.split("static esp_err_t player_play_index_at(")[1].split(
            "static esp_err_t player_play_index(")[0]
        self.assertLess(start.index("player_stop_playback();"),
                        start.index("solar_os_task_create_pinned_external("))
        restart = video.split("if (vplay_state.restart && (!vplay_state.player || vplay_state.player->done))")[1].split(
            "mpeg_player_event(ctx,")[0]
        self.assertLess(restart.index("mpeg_player_destroy("), restart.index("mpeg_player_start("))
        self.assertIn("vplay_state.restart_paused", restart)
        self.assertIn("vplay_state.restart_time", restart)

    def test_prefix_decoding_yields_and_progress_rebases(self):
        audio = (ROOT / "src/services/solar_os_audio.c").read_text()
        video = (ROOT / "src/apps/solar_os_vplay.c").read_text()
        prefix = audio.split("if (frame.frames == 0U)")[1].split("continue;")[0]
        self.assertIn("vTaskDelay(1)", prefix)
        self.assertIn("p->seeking && esp_timer_get_time() - p->seek_yield_at", video)
        self.assertIn('return "SEEKING"', (ROOT / "src/apps/solar_os_player.c").read_text())
        self.assertIn("p->sink_frames = (uint64_t)(p->start_time * p->sample_rate)", video)
        self.assertIn("esp_timer_get_time() - (int64_t)(p->start_time * 1000000)", video)


if __name__ == "__main__":
    unittest.main()
