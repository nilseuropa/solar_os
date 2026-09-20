from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMMAND = (ROOT / "src/shell/solar_os_shell_modem.c").read_text(
    encoding="utf-8"
)
TUI = (ROOT / "src/shell/solar_os_shell_modem_tui.c").read_text(
    encoding="utf-8"
)
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(
    encoding="utf-8"
)


class ModemTuiTest(unittest.TestCase):
    def test_bare_command_launches_tui_and_list_remains_textual(self):
        start = COMMAND.index("void solar_os_shell_cmd_modem(")
        command = COMMAND[start:]
        self.assertIn("if (argc == 1)", command)
        self.assertIn("solar_os_shell_launch_modem_tui(ctx)", command)
        self.assertIn('strcmp(argv[1], "list") == 0', command)
        self.assertLess(
            command.index("solar_os_shell_launch_modem_tui(ctx)"),
            command.index('strcmp(argv[1], "list") == 0'),
        )

    def test_tui_exposes_status_settings_and_actions(self):
        self.assertIn('static const char status_label[] = " Status ";', TUI)
        self.assertIn('static const char settings_label[] = " Settings ";', TUI)
        self.assertIn("solar_os_modem_set_power", TUI)
        self.assertIn("solar_os_modem_reset", TUI)
        self.assertIn("solar_os_modem_set_data_active", TUI)
        self.assertIn("solar_os_modem_profile_set", TUI)
        self.assertIn("solar_os_modem_transport_rate_set", TUI)

    def test_password_is_redacted_and_masked(self):
        self.assertIn('"password     %s"', TUI)
        self.assertIn('modem_tui.profile.password[0] != \'\\0\' ? "set" : "none"', TUI)
        self.assertIn("solar_os_tui_draw_input_ex", TUI)
        self.assertIn("masked);", TUI)

    def test_package_builds_modem_tui(self):
        package_start = PACKAGES.index("[packages.service_modem]")
        package_end = PACKAGES.index("\n[packages.", package_start + 1)
        package = PACKAGES[package_start:package_end]
        self.assertIn('"shell/solar_os_shell_modem_tui.c"', package)


if __name__ == "__main__":
    unittest.main()
