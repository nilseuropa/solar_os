import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")
APPS = (ROOT / "src/apps/solar_os_app_registry.c").read_text(encoding="utf-8")
JOBS = (ROOT / "src/jobs/solar_os_job_registry.c").read_text(encoding="utf-8")
DESCRIPTOR = (ROOT / "src/apps/solar_os_script_api.inc").read_text(encoding="utf-8")
SERVICE = (ROOT / "src/services/solar_os_ftp.c").read_text(encoding="utf-8")
DAEMON = (ROOT / "src/jobs/solar_os_ftpd_job.c").read_text(encoding="utf-8")
FTP_APP = (ROOT / "src/apps/solar_os_ftp_app.c").read_text(encoding="utf-8")


class FtpFeatureTest(unittest.TestCase):
    def test_service_app_and_job_are_independent_packages(self):
        self.assertIn("[packages.service_ftp]", PACKAGES)
        self.assertIn("[packages.app_ftp]", PACKAGES)
        self.assertIn('depends = ["service_ftp"]', PACKAGES)
        self.assertIn("[packages.job_ftpd]", PACKAGES)
        self.assertIn('APP_ENTRY("ftp"', APPS)
        self.assertIn('{"ftpd", "FTP file server"', JOBS)

    def test_client_uses_passive_mode_and_binary_transfers(self):
        self.assertIn('ftp_command(session, &code, "EPSV")', SERVICE)
        self.assertIn('ftp_command(session, &code, "PASV")', SERVICE)
        self.assertIn('ftp_command(session, &code, "TYPE I")', SERVICE)
        self.assertIn('ftp_begin_data_command(session, &data_fd, "RETR"', SERVICE)
        self.assertIn('ftp_begin_data_command(session, &data_fd, "STOR"', SERVICE)
        self.assertIn("solar_os_storage_replace_file", SERVICE)

    def test_daemon_confines_paths_to_export_root(self):
        self.assertIn("ftpd_normalize_virtual", DAEMON)
        self.assertIn('snprintf(local_path, local_len, "%s%s", ftpd.root, virtual_path)', DAEMON)
        self.assertIn('strcmp(virtual_path, "/") == 0', DAEMON)
        self.assertIn('"anonymous login"', DAEMON)
        self.assertIn("solar_os_storage_replace_file", DAEMON)

    def test_python_and_lua_share_typed_ftp_surface(self):
        for method in ("list", "download", "upload", "mkdir", "rmdir", "remove", "rename"):
            self.assertIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(ftp, {method}, {method});",
                DESCRIPTOR,
            )

    def test_app_reports_transfer_progress_and_preserves_pane_position(self):
        self.assertIn("solar_os_tui_progress_bar", FTP_APP)
        self.assertIn("xQueueOverwrite(ftp_app.progress_events", FTP_APP)
        self.assertIn("ftp_app_restore_position", FTP_APP)
        self.assertIn("same_remote_path", FTP_APP)


if __name__ == "__main__":
    unittest.main()
