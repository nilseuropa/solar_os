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

    def test_rejected_login_reopens_the_password_field(self):
        self.assertIn("static esp_err_t ftp_expect_authentication", SERVICE)
        self.assertIn("code == 430 || code == 530", SERVICE)
        self.assertIn("return ESP_ERR_INVALID_CRC", SERVICE)
        poll = FTP_APP.split("static void ftp_app_poll", 1)[1]
        poll = poll.split("static void ftp_app_open_selected", 1)[0]
        self.assertIn("result.error == ESP_ERR_INVALID_CRC", poll)
        self.assertIn("ftp_app.connection.field = FTP_APP_CONNECT_PASSWORD", poll)

    def test_connection_failures_keep_human_readable_socket_diagnostics(self):
        self.assertIn("char *error;", (ROOT / "src/services/solar_os_ftp.h").read_text())
        self.assertIn("connect_error == ECONNREFUSED", SERVICE)
        self.assertIn('const char *message = "server unavailable"', SERVICE)
        self.assertIn('message = "connection refused"', SERVICE)
        self.assertIn('message = "host unreachable"', SERVICE)
        self.assertIn('message = "network unreachable"', SERVICE)
        self.assertIn(".error = result.detail", FTP_APP)
        self.assertIn(
            "result.detail[0] != '\\0' ?\n"
            "                                    result.detail : esp_err_to_name(result.error)",
            FTP_APP,
        )

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

    def test_app_starts_disconnected_and_matches_files_operation_mnemonics(self):
        self.assertIn('"F2 opens the connection setup"', FTP_APP)
        self.assertIn("case SOLAR_OS_KEY_F2:", FTP_APP)
        self.assertIn(
            '"F2 Connect F3 View F5 Copy F6 Move F7 mKdir F8 Delete"',
            FTP_APP,
        )
        self.assertIn('{"Connect", 2U}', FTP_APP)
        connect_cases = FTP_APP.split("case SOLAR_OS_KEY_F2:", 1)[1].split(
            "ftp_app_begin_connection();", 1
        )[0]
        self.assertIn("case 'n':", connect_cases)
        self.assertIn("case 'N':", connect_cases)
        for binding in ("case 'V':", "case 'C':", "case 'M':", "case 'k':", "case 'K':", "case 'D':"):
            self.assertIn(binding, FTP_APP)

        event = FTP_APP.split("static bool ftp_app_event", 1)[1]
        event = event.split("static void ftp_app_release_cleanup", 1)[0]
        up_cases = event.split("case SOLAR_OS_KEY_UP:", 1)[1].split(
            "case SOLAR_OS_KEY_DOWN:", 1
        )[0]
        down_cases = event.split("case SOLAR_OS_KEY_DOWN:", 1)[1].split(
            "case SOLAR_OS_KEY_HOME:", 1
        )[0]
        self.assertNotIn("case 'k':", up_cases)
        self.assertNotIn("case 'j':", down_cases)

    def test_host_argument_keeps_the_existing_immediate_connect_behavior(self):
        parser = FTP_APP.split("static esp_err_t ftp_app_parse_args", 1)[1]
        parser = parser.split("static esp_err_t ftp_app_start", 1)[0]
        self.assertIn("*connect_immediately = argc >= 2", parser)
        self.assertIn(
            "strlcpy(ftp_app.host, solar_os_context_argv(ctx, 1), sizeof(ftp_app.host))",
            parser,
        )


if __name__ == "__main__":
    unittest.main()
