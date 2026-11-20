from pathlib import Path
import unittest
from tests.test_foreground_presenter import production_functions


ROOT = Path(__file__).resolve().parents[1]
JOB = (ROOT / "src/jobs/solar_os_rtspd_job.c").read_text(encoding="utf-8")
REGISTRY = (ROOT / "src/jobs/solar_os_job_registry.c").read_text(encoding="utf-8")
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")
JOBS_MANUAL = (ROOT / "doc/manual/jobs.reference.md").read_text(encoding="utf-8")


class RtspdJobPolicyTest(unittest.TestCase):
    def test_live_stream_completion_is_package_gated_and_keeps_equals_open(self):
        shell = (ROOT / "src/apps/solar_os_shell.c").read_text()
        self.assertIn("#if SOLAR_OS_PACKAGE_JOB_RTSPD\n    if (solar_os_shell_rtspd_completion_emit", shell)
        self.assertIn("solar_os_stream_count(), shell_completion_get_rtspd_stream", shell)
        complete = production_functions(shell, ("shell_complete_argument",))
        self.assertIn("solar_os_shell_completion_needs_trailing_space(match)", complete)
        self.assertIn('"shell/solar_os_shell_rtspd_completion.c"', PACKAGES)

    def test_package_and_registry_wiring(self):
        package = PACKAGES.split("[packages.job_rtspd]", 1)[1].split(
            "[packages.", 1
        )[0]
        self.assertIn('label = "job.rtspd"', package)
        self.assertNotIn('"service_camera"', package)
        self.assertIn('"service_streams"', package)
        self.assertIn('capabilities = ["wifi"]', package)
        self.assertIn('"service_media"', package)
        self.assertIn('"jobs/solar_os_rtspd_job.c"', package)
        self.assertIn("SOLAR_OS_PACKAGE_JOB_RTSPD", REGISTRY)
        self.assertIn('{"rtspd", "RTSP/RTP media publisher"', REGISTRY)

    def test_job_owns_camera_and_uses_standard_media_core(self):
        self.assertIn('#define RTSPD_OWNER "job:rtspd"', JOB)
        self.assertIn("solar_os_stream_open_ex(rtspd.options.video_source", JOB)
        self.assertIn("solar_os_stream_acquire_frame", JOB)
        self.assertNotIn("solar_os_camera_capture", JOB)
        self.assertIn("solar_os_rtp_jpeg_parse", JOB)
        self.assertIn("solar_os_rtp_jpeg_packetize", JOB)
        self.assertIn("solar_os_rtcp_sender_report", JOB)
        self.assertIn("SOLAR_OS_MEDIA_RTP_PACKET_MAX", JOB)

    def test_single_client_and_bounded_stop_are_explicit(self):
        self.assertIn("rtspd_reject_pending_client", JOB)
        self.assertIn("solar_os_task_wait_done", JOB)
        self.assertIn("solar_os_stream_release_frame", JOB)
        self.assertIn("RTSP/1.0 453 Not Enough Bandwidth", JOB)

    def test_bulk_buffers_are_runtime_policy_allocations(self):
        self.assertNotIn("static rtspd_session_t rtspd_session;", JOB)
        self.assertIn('SOLAR_OS_MEMORY_EXTERNAL_PREFERRED, "rtspd.session"', JOB)
        self.assertIn("solar_os_memory_free(rtspd.session)", JOB)
        reader = JOB.split("static void rtspd_audio_reader", 1)[1].split(
            "static void rtspd_send_rtcp", 1
        )[0]
        self.assertNotIn("int16_t samples[", reader)
        self.assertNotIn("uint8_t packet[", reader)
        self.assertIn("rtspd_audio_scratch_t *scratch", reader)
        self.assertIn("uxTaskGetStackHighWaterMark(NULL)", JOB)

    def test_manual_documents_rtsp_url_and_security(self):
        section = JOBS_MANUAL.split("## rtspd", 1)[1].split("\n## ", 1)[0]
        self.assertIn("rtsp://device/media", section)
        self.assertIn("single-client RTSP", section)
        self.assertIn("unauthenticated and unencrypted", section)


if __name__ == "__main__":
    unittest.main()
