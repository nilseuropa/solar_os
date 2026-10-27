from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]
APP = (ROOT / "src/apps/solar_os_rtsp_app.c").read_text()
CLIENT = (ROOT / "src/services/solar_os_rtsp_client.c").read_text()
PACKAGES = tomllib.loads((ROOT / "packages/solar_os_packages.toml").read_text())


class RtspAppTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("cc"), "requires C compiler")
    def test_production_layout_and_transport_helpers(self):
        # Compile the real state and selected production helpers, avoiding
        # copies of their logic and dependence on an ESP-IDF host environment.
        state = APP[APP.index("typedef struct {"):APP.index("static void rtsp_samples")]
        constants = "\n".join(re.findall(r"^#define RTSP_.*$", APP, re.MULTILINE))
        helpers = (
            APP[APP.index("static void playback_status"):APP.index("static void diagnostics_tick")]
            + APP[APP.index("static esp_err_t start_playback"):APP.index("static esp_err_t start(")]
            + APP[APP.index("static bool pointer("):APP.index("const solar_os_app_t solar_os_rtsp_app")]
        )
        with tempfile.TemporaryDirectory(prefix="solaros-rtsp-ui-") as directory:
            tmp = Path(directory)
            (tmp / "rtsp_app_test_state.h").write_text(constants + "\n" + state)
            (tmp / "rtsp_app_test_code.h").write_text(helpers)
            binary = tmp / "rtsp_app_ui_test"
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-D_POSIX_C_SOURCE=200809L",
                "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                f"-I{tmp}", f"-I{ROOT / 'tests/host'}", f"-I{ROOT / 'src'}",
                f"-I{ROOT / 'src/services'}", "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/rtsp_app_ui_test.c"),
                str(ROOT / "src/services/solar_os_rtsp_receiver.c"),
                str(ROOT / "src/services/solar_os_rtsp.c"),
                str(ROOT / "src/services/solar_os_rtp.c"),
                str(ROOT / "src/services/solar_os_media.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_address_shorthand_is_normalized_before_client_creation(self):
        start = APP.split("static esp_err_t start(", 1)[1].split("static void stop(", 1)[0]
        self.assertLess(start.index("solar_os_rtsp_url_normalize"),
                        start.index("start_playback(ctx)"))
        self.assertIn("solar_os_rtsp_client_create(rtsp.url", APP)
        self.assertNotIn("strcpy(rtsp.url, url)", start)
        registry = (ROOT / "src/apps/solar_os_app_registry.c").read_text()
        self.assertIn("<[rtsp://][user[:pass]@]host[:port][/path]>", registry)
        manual = (ROOT / "doc/manual/apps.md").read_text()
        self.assertIn("rtsp 192.168.1.238/media", manual)
        self.assertIn("TCP port 554", manual)

    def test_registry_packages_and_user_manual(self):
        self.assertIn("app_rtsp", PACKAGES["groups"]["rtsp"]["members"])
        self.assertIn("service_rtsp_client", PACKAGES["packages"]["app_rtsp"]["depends"])
        self.assertIn("service_media", PACKAGES["packages"]["service_rtsp_client"]["depends"])
        self.assertIn("service_audio", PACKAGES["packages"]["service_rtsp_client"]["depends"])
        registry = (ROOT / "src/apps/solar_os_app_registry.c").read_text()
        self.assertIn('APP_ENTRY("rtsp"', registry)
        self.assertIn("SOLAR_OS_APP_CAP_PORT", registry.split('APP_ENTRY("rtsp"', 1)[1].split("\n", 1)[0])
        manual = (ROOT / "doc/manual/apps.md").read_text().split("## rtsp", 1)[1].split("## ", 1)[0]
        self.assertIn("--audio-only", manual)
        self.assertIn("oscilloscope", manual)
        self.assertIn("TCP interleaving", manual)

    def test_audio_only_uses_common_scope_and_video_uses_common_blitter(self):
        self.assertIn("solar_os_oscilloscope_widget_draw", APP)
        self.assertIn("solar_os_oscilloscope_widget_submit_s16", APP)
        self.assertIn("solar_os_gfx_blit_raster", APP)
        self.assertIn("solar_os_media_transport_button_draw", APP)
        self.assertIn(".video = rtsp.graphical && !rtsp.audio_only", APP)
        self.assertIn(".samples = audio_samples", CLIENT)

    def test_cold_state_and_worker_ownership(self):
        self.assertIn(".state_slot = &rtsp_state", APP)
        self.assertIn(".state_release_ready = release_ready", APP)
        self.assertIn("solar_os_task_wait_done", APP)
        self.assertNotIn("solar_os_camera_", APP + CLIENT)
        self.assertNotIn("solar_os_stb_", CLIENT)
        self.assertIn('SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, "rtsp.jpeg"', CLIENT)
        self.assertIn("solar_os_audio_s16_convert", CLIENT)
        self.assertIn("c->leased || c->pending", CLIENT)
        self.assertIn("CLIENT_TIMEOUT_US", CLIENT)

    def test_bounded_decode_ahead_and_frame_driven_presentation(self):
        self.assertIn("#define RTSP_VIDEO_SLOTS 2U", APP)
        self.assertIn("bool full = rtsp.queued == RTSP_VIDEO_SLOTS", APP)
        self.assertIn("if (full) { vTaskDelay", APP)
        self.assertIn("solar_os_rtsp_client_video_lateness", APP)
        self.assertIn("if (late < 0) break", APP)
        self.assertIn("if (late > 150000)", APP)
        self.assertIn("!changed && !dirty && !force", APP)
        self.assertIn("SOLAR_OS_MEMORY_EXTERNAL_REQUIRED, \"rtsp.image\"", APP)
        self.assertIn("for (unsigned i = 0; i < rtsp.queued; i++) solar_os_memory_free", APP)
        self.assertIn("prepare_image(pixels", APP)

    def test_opt_in_diagnostics_and_port_safe_sampling(self):
        self.assertIn('!strcmp(arg, "--stats")', APP)
        self.assertIn("if (!rtsp.diagnostics) return", APP)
        self.assertIn(".diagnostics = rtsp.diagnostics", APP)
        self.assertIn("if (rtsp.image_mutex) xSemaphoreTake", APP)
        self.assertIn('SOLAR_OS_LOGI("rtsp.stats"', APP)
        self.assertIn("audio_gap_max_us", CLIENT)
        self.assertIn("blit_max_us", APP)
        self.assertIn("present_max_us", APP)
        self.assertIn("audio_concealed_frames", CLIENT)
        registry = (ROOT / "src/apps/solar_os_app_registry.c").read_text()
        entry = registry.split('APP_ENTRY("rtsp"', 1)[1].split("\n", 1)[0]
        self.assertIn("[--audio-only] [--stats]", entry)
        self.assertIn(", 2, 4)", entry)

    def test_monochrome_frames_stay_gray_and_color_retains_rgb(self):
        self.assertIn("solar_os_stb_jpeg_decode_gray", APP)
        self.assertIn("solar_os_stb_decode_jpeg_rgb_scaled", APP)
        self.assertIn("SOLAR_OS_GFX_RASTER_GRAY8 : SOLAR_OS_GFX_RASTER_RGB888", APP)
        self.assertIn("sy == previous_sy", APP)
        start = APP.split("static esp_err_t start(", 1)[1].split("static void stop(", 1)[0]
        self.assertLess(start.index("solar_os_context_set_graphics_active(ctx, true)"),
                        start.index("rtsp.monochrome = solar_os_gfx_format"))

    def test_common_player_fullscreen_volume_and_frame_overlay(self):
        self.assertIn("if (chrome && !rtsp.fullscreen) draw_controls", APP)
        self.assertIn("SOLAR_OS_GFX_FONT_BOLD_16", APP)
        self.assertIn('"VOL"', APP)
        self.assertIn("h - RTSP_CONTROLS_HEIGHT", APP)
        self.assertIn("rtsp.stopped ? SOLAR_OS_MEDIA_TRANSPORT_PLAY : SOLAR_OS_MEDIA_TRANSPORT_STOP", APP)
        self.assertNotIn("RTSP_HELP_HEIGHT", APP)
        self.assertIn("rtsp.fullscreen = !rtsp.fullscreen", APP)
        self.assertIn("generation != rtsp.layout_generation", APP)
        self.assertIn("key == SOLAR_OS_KEY_UP || key == SOLAR_OS_KEY_DOWN", APP)
        self.assertIn("rtsp.frame_diagnostics = !rtsp.frame_diagnostics", APP)
        self.assertNotIn("volume %u%%  frames", APP)

    def test_scope_uses_the_entire_viewport_including_fullscreen(self):
        self.assertIn("int content_height = rtsp.fullscreen ? h :", APP)
        self.assertIn("const int margin = rtsp.fullscreen ? 0 : 3", APP)
        self.assertIn("w - 2 * margin, content_height - 2 * margin", APP)
        self.assertNotIn("content_height * 2 / 3", APP)

    def test_stop_reconnect_waits_for_both_workers_and_releases_owners(self):
        reap = APP.split("static void reap_playback(void)", 1)[1].split("static void stop_playback", 1)[0]
        self.assertIn("if (!rtsp.network_done || !rtsp.decode_done) return", reap)
        self.assertIn("solar_os_task_delete_external(rtsp.network_task)", reap)
        self.assertIn("solar_os_task_delete_external(rtsp.decode_task)", reap)
        self.assertIn("solar_os_rtsp_client_destroy(rtsp.client)", reap)
        stop = APP.split("static void stop_playback", 1)[1].split("static void toggle_playback", 1)[0]
        self.assertIn("solar_os_rtsp_client_cancel", stop)
        self.assertIn("rtsp.layout_generation++", stop)
        self.assertIn("rtsp.queued = 0", stop)
        self.assertNotIn("solar_os_task_wait_done", stop)
        event = APP.split("static bool event(", 1)[1]
        self.assertLess(event.index("if (rtsp.stopped)"), event.index("if (rtsp.network_done)"))
        self.assertIn("rtsp.restart && !rtsp.client", event)
        self.assertIn("key == SOLAR_OS_KEY_ENTER || key == ' '", event)
        self.assertIn("SOLAR_OS_EVENT_POINTER", event)
        self.assertIn("SOLAR_OS_APP_FLAG_RESUMABLE | SOLAR_OS_APP_FLAG_POINTER_EVENTS", APP)
        self.assertIn("rtsp.fullscreen || event->action != SOLAR_OS_INPUT_POINTER_PRESS", APP)

    def test_causal_errors_are_preserved_and_shown(self):
        self.assertIn("status.error_detail", APP)
        self.assertIn("if (c->status.error == ESP_OK)", CLIENT)
        for cause in ("server closed RTSP connection", "no RTP media for 5 seconds",
                      "stream/path not found", "server rejected UDP transport", "errno %d"):
            self.assertIn(cause, CLIENT)

    def test_bounded_reconnect_and_epoch_isolation(self):
        self.assertIn(".reconnect_attempts = 6", APP)
        self.assertIn("image.epoch != status.epoch || !status.playing", APP)
        self.assertIn("failures >= c->options.reconnect_attempts", CLIENT)
        self.assertIn("if (!leased || c->cancel) break", CLIENT)
        self.assertIn("c->audio_stop = true", CLIENT)
        self.assertIn("c->status.epoch++", CLIENT)
        self.assertIn("stack free bytes", APP)
        self.assertIn("heap bytes internal=", APP)

    def test_true_color_frames_bypass_indexed_blitter(self):
        self.assertIn("solar_os_gfx_supports_frame_format", APP)
        self.assertIn("solar_os_stb_decode_jpeg_rgb565_scaled", APP)
        self.assertNotIn("solar_os_rgb565_from_rgb888", APP)
        self.assertIn("prepared = pixels", APP)
        self.assertIn("status.video && !direct", APP)
        self.assertIn("if (!direct || rtsp.layout_dirty) solar_os_gfx_clear", APP)
        self.assertIn("if (chrome) solar_os_gfx_present", APP)
        self.assertIn("solar_os_gfx_present_frame(gfx, &frame)", APP)
        tft = (ROOT / "src/drivers/tft_ili9341.c").read_text()
        self.assertIn("solar_os_rgb565_rotate", tft)
        self.assertIn("solar_os_rgb565_scale_row", tft)
        self.assertIn("rgb565 ? SOLAR_OS_MEMORY_EXTERNAL_REQUIRED", tft)


if __name__ == "__main__":
    unittest.main()
