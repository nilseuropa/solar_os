from pathlib import Path
import unittest


REPOSITORY = Path(__file__).resolve().parents[1]
WEB_SOURCE = (REPOSITORY / "src/apps/solar_os_web.c").read_text(encoding="utf-8")
PACKAGES = (REPOSITORY / "packages/solar_os_packages.toml").read_text(
    encoding="utf-8"
)
APPS_MANUAL = (REPOSITORY / "doc/manual/apps.md").read_text(encoding="utf-8")


class WebMjpegPolicyTest(unittest.TestCase):
    def test_image_package_contains_bounded_mjpeg_parser(self):
        self.assertIn('"services/solar_os_mjpeg.c"', PACKAGES)
        self.assertIn("WEB_IMAGE_MAX_BYTES", WEB_SOURCE)
        self.assertIn("solar_os_mjpeg_parser_feed", WEB_SOURCE)

    def test_http_content_type_selects_streaming_path(self):
        self.assertIn('"multipart/x-mixed-replace"', WEB_SOURCE)
        self.assertIn('web_url_ext_eq(dot, end, ".mjpeg")', WEB_SOURCE)
        self.assertIn("web_prepare_mjpeg_worker(&worker)", WEB_SOURCE)
        self.assertIn("worker->mjpeg", WEB_SOURCE)
        self.assertIn(".read_poll_ms = 250U", WEB_SOURCE)
        self.assertIn(".cancel_flag = &web.stop_requested", WEB_SOURCE)
        header_handler = WEB_SOURCE.split(
            "static esp_err_t web_http_event", 1
        )[1].split("static bool web_line_empty", 1)[0]
        self.assertNotIn("event->status_code >= 200", header_handler)

    def test_ui_handoff_is_one_slot_and_drops_stale_frames(self):
        self.assertIn(
            "solar_os_queue_create(1U, sizeof(web_mjpeg_frame_t))", WEB_SOURCE
        )
        publish = WEB_SOURCE.split("static bool web_publish_mjpeg_frame", 1)[1].split(
            "static void web_discard_pending_mjpeg_frame", 1
        )[0]
        self.assertIn("xQueueReceive(web.mjpeg_frames, &stale, 0)", publish)
        self.assertIn("web_free_mjpeg_frame(&stale)", publish)
        self.assertIn("worker->dropped_frames++", publish)

    def test_navigation_cancels_live_stream(self):
        cancel = WEB_SOURCE.split("static bool web_stop_worker_for_navigation", 1)[
            1
        ].split("static esp_err_t web_start_load", 1)[0]
        self.assertIn("web_cancel_request();", cancel)
        self.assertIn("solar_os_task_wait_done", cancel)

    def test_manual_documents_direct_mjpeg_playback(self):
        self.assertIn("web http://camera-host/camera.mjpeg", APPS_MANUAL)
        self.assertIn("single pending", APPS_MANUAL)
        self.assertIn("drops the stale pending frame", APPS_MANUAL)


if __name__ == "__main__":
    unittest.main()
