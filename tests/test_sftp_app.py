import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(encoding="utf-8")
APPS = (ROOT / "src/apps/solar_os_app_registry.c").read_text(encoding="utf-8")
FTP_APP = (ROOT / "src/apps/solar_os_ftp_app.c").read_text(encoding="utf-8")
SFTP_APP = (ROOT / "src/apps/solar_os_sftp_app.c").read_text(encoding="utf-8")
SERVICE = (ROOT / "src/services/solar_os_sftp.c").read_text(encoding="utf-8")
MANUAL = (ROOT / "doc/manual/apps.md").read_text(encoding="utf-8")


class SftpAppTest(unittest.TestCase):
    def test_reuses_ftp_file_manager_interaction_model(self):
        self.assertIn('#include "solar_os_ftp_app.c"', SFTP_APP)
        self.assertIn('#define FTP_APP_COMMAND "sftp"', SFTP_APP)
        self.assertIn("FTP_APP_INPUT_CONNECT", FTP_APP)
        self.assertIn("case SOLAR_OS_KEY_F2:", FTP_APP)
        self.assertIn("case 'n':", FTP_APP)

    def test_user_host_path_shortcut_connects_immediately(self):
        parser = FTP_APP.split("static esp_err_t ftp_app_parse_args", 1)[1]
        parser = parser.split("static esp_err_t ftp_app_start", 1)[0]
        self.assertIn("*connect_immediately = argc >= 2", parser)
        self.assertIn("ftp_app_parse_sftp_target", parser)
        target = FTP_APP.split("static bool ftp_app_parse_sftp_target", 1)[1]
        target = target.split("static esp_err_t ftp_app_parse_args", 1)[0]
        self.assertIn("const char *colon = strchr(target, ':')", target)
        self.assertIn("strlcpy(remote_path, colon + 1", target)

    def test_connection_form_restores_the_active_input_cursor(self):
        dialog = FTP_APP.split("static void ftp_app_draw_connection", 1)[1]
        dialog = dialog.split("static void ftp_app_draw_operation_help", 1)[0]
        help_position = dialog.index('"Tab field  Enter connect  Esc cancel"')
        restore_position = dialog.index("ftp_app_draw_connection_field", help_position)
        self.assertGreater(restore_position, help_position)

    def test_authentication_failure_reopens_password_field(self):
        poll = FTP_APP.split("static void ftp_app_poll", 1)[1]
        poll = poll.split("static void ftp_app_open_selected", 1)[0]
        self.assertIn("result.error == ESP_ERR_INVALID_CRC", poll)
        self.assertIn("ftp_app_begin_connection();", poll)
        self.assertIn("ftp_app.connection.field = FTP_APP_CONNECT_PASSWORD", poll)
        self.assertIn('"authentication failed; enter password"', poll)

    def test_service_uses_shared_ssh_transport_and_sftp_subsystem(self):
        self.assertIn("solar_os_ssh_transport_open", SERVICE)
        self.assertIn("libssh2_sftp_init", SERVICE)
        self.assertIn("libssh2_sftp_readdir", SERVICE)
        self.assertIn("solar_os_storage_replace_file", SERVICE)
        self.assertIn("sftp_rename_internal", SERVICE)

    def test_package_and_registry_wiring(self):
        service = PACKAGES.split("[packages.service_sftp]", 1)[1].split("\n[", 1)[0]
        app = PACKAGES.split("[packages.app_sftp]", 1)[1].split("\n[", 1)[0]
        self.assertIn('depends = ["service_ssh"]', service)
        self.assertIn('sources = ["services/solar_os_sftp.c"]', service)
        self.assertIn('depends = ["service_sftp"]', app)
        self.assertIn('sources = ["apps/solar_os_sftp_app.c"]', app)
        self.assertIn('APP_ENTRY("sftp"', APPS)

    def test_manual_documents_connection_form_and_matching_controls(self):
        section = MANUAL.split("## sftp", 1)[1].split("\n## ", 1)[0]
        self.assertIn("Press `F2`", section)
        self.assertIn("`sftp nils@remote:/directory`", section)
        self.assertIn("reopens the form on the password field", section)
        for binding in ("`F3`/`V`", "`F5`/`C`", "`F6`/`M`", "`F7`/`k`/`K`", "`F8`/`D`"):
            self.assertIn(binding, section)


if __name__ == "__main__":
    unittest.main()
