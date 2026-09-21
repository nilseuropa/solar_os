import pathlib
import unittest


REPOSITORY = pathlib.Path(__file__).resolve().parents[1]
APP = (REPOSITORY / "src/apps/solar_os_sftpsync.c").read_text(encoding="utf-8")
SERVICE = (REPOSITORY / "src/services/solar_os_sftpsync.c").read_text(encoding="utf-8")
HEADER = (REPOSITORY / "src/services/solar_os_sftpsync.h").read_text(encoding="utf-8")
SHELL = (REPOSITORY / "src/apps/solar_os_shell.c").read_text(encoding="utf-8")
MANUAL = (REPOSITORY / "doc/manual/apps.md").read_text(encoding="utf-8")
PACKAGES = (REPOSITORY / "packages/solar_os_packages.toml").read_text(encoding="utf-8")
DESCRIPTOR = (REPOSITORY / "src/apps/solar_os_script_api.inc").read_text(encoding="utf-8")
PYTHON = (REPOSITORY / "src/apps/solar_os_python_sftpsync.inc").read_text(encoding="utf-8")
LUA = (REPOSITORY / "src/apps/solar_os_lua_sftpsync.inc").read_text(encoding="utf-8")


class SftpsyncAppTest(unittest.TestCase):
    def test_supports_push_pull_recursive_and_dry_run(self):
        self.assertIn("SOLAR_OS_SFTPSYNC_UPLOAD", APP)
        self.assertIn("SOLAR_OS_SFTPSYNC_DOWNLOAD", APP)
        self.assertIn('strcmp(arg, "--recursive")', APP)
        self.assertIn('strcmp(arg, "--dry-run")', APP)
        self.assertIn("parsed_recursive", APP)
        self.assertIn("parsed_dry_run", APP)

    def test_accepts_clustered_short_options(self):
        parser = APP.split("static bool sftpsync_parse_short_options", 1)[1]
        parser = parser.split("static bool sftpsync_parse_args", 1)[0]
        self.assertIn("for (const char *option = arg + 1", parser)
        self.assertIn("*option == 'r' || *option == 'a'", parser)
        self.assertIn("*option == 'n'", parser)
        parse_args = APP.split("static bool sftpsync_parse_args", 1)[1]
        parse_args = parse_args.split("static void sftpsync_usage", 1)[0]
        self.assertIn("} else if (arg[0] == '-') {", parse_args)
        self.assertNotIn("arg[0] == '-' &&", parse_args)

    def test_uses_sftp_quick_check_and_staged_files(self):
        self.assertIn("libssh2_sftp_init", SERVICE)
        self.assertIn("remote->filesize", SERVICE)
        self.assertIn("remote->mtime", SERVICE)
        self.assertIn(".solaros-sftpsync-part", SERVICE)
        self.assertIn("sftpsync_remote_rename", SERVICE)

    def test_shell_resolves_local_operand_before_launch(self):
        launch = SHELL.split("static bool shell_prepare_ssh_copy_launch_args", 1)[1]
        launch = launch.split("static bool shell_prepare_app_launch_args", 1)[0]
        self.assertIn('strcmp(command, "sftpsync")', launch)
        self.assertIn("solar_os_shell_resolve_path_for_command", launch)

    def test_ctrl_c_and_app_exit_cancel(self):
        event = APP.split("static bool sftpsync_event", 1)[1]
        self.assertIn("SOLAR_OS_KEY_APP_EXIT", event)
        self.assertIn("(uint8_t)ch == 0x03U", event)
        self.assertIn("solar_os_sftpsync_stop", event)

    def test_reports_per_file_progress_bar(self):
        for field in ("file_transferred", "file_size", "file_size_known"):
            self.assertIn(field, HEADER)
        self.assertIn("session->file_transferred +=", SERVICE)
        self.assertIn("sftpsync_progress_render", APP)
        self.assertIn("solar_os_shell_io_redraw_line", APP)
        self.assertIn("progress bar for each", MANUAL)

    def test_sftpsync_package_wiring(self):
        service = PACKAGES.split("[packages.service_sftpsync]", 1)[1].split("\n[", 1)[0]
        app = PACKAGES.split("[packages.app_sftpsync]", 1)[1].split("\n[", 1)[0]
        self.assertIn('sources = ["services/solar_os_sftpsync.c"]', service)
        self.assertIn('depends = ["service_sftpsync"]', app)

    def test_python_and_lua_share_typed_sync_surface(self):
        self.assertIn(
            "SOLAR_OS_SCRIPT_API_FUNCTION(sftpsync, sync, sync);",
            DESCRIPTOR,
        )
        self.assertIn("solaros_sftpsync_sync_obj", PYTHON)
        self.assertIn("solua_sftpsync_sync", LUA)
        for binding in (PYTHON, LUA):
            self.assertIn("files_changed", binding)
            self.assertIn("bytes_transferred", binding)
            self.assertIn("solar_os_sftpsync_stop", binding)

    def test_manual_states_interoperability_and_delete_policy(self):
        section = MANUAL.split("## sftpsync", 1)[1].split("\n## ", 1)[0]
        self.assertIn("SFTP subsystem", section)
        self.assertIn("does not delete destination-only files", section)
        self.assertIn("Ctrl+C", section)


if __name__ == "__main__":
    unittest.main()
