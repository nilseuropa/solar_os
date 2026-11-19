import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")
REGISTRY = (ROOT / "src/jobs/solar_os_job_registry.c").read_text(encoding="utf-8")
JOB = (ROOT / "src/jobs/solar_os_cam_webd_job.c").read_text(encoding="utf-8")
MANUAL = (ROOT / "doc/manual/jobs.reference.md").read_text(encoding="utf-8")


class CameraWebJobTest(unittest.TestCase):
    def test_camera_group_installs_job_and_dependencies(self):
        self.assertIn(
            'members = ["driver_camera_esp32", "driver_himax_hx6538", "job_cam_webd"]',
            PACKAGES,
        )
        package = PACKAGES.split("[packages.job_cam_webd]", 1)[1].split(
            "[packages.job_displayd]", 1
        )[0]
        self.assertIn('depends = ["service_camera", "service_http_server"]', package)
        self.assertIn('capabilities = ["psram", "wifi"]', package)
        self.assertIn('targets = ["esp32s3"]', package)
        self.assertIn(
            '{"cam-webd", "HTTP camera stream", &solar_os_cam_webd_job}',
            REGISTRY,
        )

    def test_stream_uses_async_request_and_exact_frame_lease(self):
        stream_route = JOB.split('.uri = "/camera.mjpeg"', 1)[1].split("};", 1)[0]
        self.assertIn(".asynchronous = true", stream_route)
        self.assertIn("solar_os_http_server_complete_async(req)", JOB)
        self.assertIn("solar_os_camera_capture(&cam_webd.camera_owner, &frame)", JOB)
        self.assertIn("solar_os_camera_release_frame(&cam_webd.camera_owner, frame)", JOB)
        self.assertNotIn("solar_os_memory_alloc", JOB)

    def test_camera_is_exclusively_owned_for_job_lifetime(self):
        self.assertIn(
            "solar_os_camera_acquire(CAM_WEBD_ROUTE_OWNER,\n"
            "                                              &cam_webd.camera_owner)",
            JOB,
        )
        self.assertIn("solar_os_camera_stop(&cam_webd.camera_owner)", JOB)
        self.assertIn("solar_os_camera_release_owner(&cam_webd.camera_owner)", JOB)
        self.assertIn("!cam_webd.stream_active && cam_webd.worker_task == NULL", JOB)

    def test_routes_default_to_public_with_optional_authentication(self):
        self.assertIn("bool auth_required = false;", JOB)
        self.assertIn('strcmp(text, "auth=none") == 0', JOB)
        self.assertIn('strcmp(text, "auth=required") == 0', JOB)
        self.assertIn(
            "cam_webd.auth_required ?\n"
            "        SOLAR_OS_HTTP_AUTH_VIEW : SOLAR_OS_HTTP_AUTH_PUBLIC",
            JOB,
        )
        self.assertEqual(JOB.count(".auth = auth"), 3)
        self.assertIn("WARNING: unauthenticated camera access", JOB)
        self.assertIn("with no authentication", MANUAL)
        self.assertIn("auth=required", MANUAL)

    def test_stream_limits_and_security_are_documented(self):
        self.assertIn("Only one stream client is admitted", MANUAL)
        self.assertIn("there is no frame queue or JPEG copy", MANUAL)
        self.assertIn("does not encrypt images", MANUAL)


if __name__ == "__main__":
    unittest.main()
