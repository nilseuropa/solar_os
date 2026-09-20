from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMMAND = (ROOT / "src/shell/solar_os_shell_expansion.c").read_text(
    encoding="utf-8"
)
TUI = (ROOT / "src/shell/solar_os_shell_expansion_tui.c").read_text(
    encoding="utf-8"
)
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(
    encoding="utf-8"
)


class ExpansionTuiTest(unittest.TestCase):
    def test_bare_command_launches_tui_and_status_remains_textual(self):
        start = COMMAND.index("void solar_os_shell_cmd_expansion(")
        command = COMMAND[start:]

        self.assertIn("if (argc == 1)", command)
        self.assertIn("solar_os_shell_launch_expansion_tui(ctx)", command)
        self.assertIn('strcmp(argv[1], "status") == 0', command)
        self.assertLess(
            command.index("solar_os_shell_launch_expansion_tui(ctx)"),
            command.index('strcmp(argv[1], "status") == 0'),
        )

    def test_tui_manages_devices_and_browses_driver_categories(self):
        self.assertIn("solar_os_expansion_device_count()", TUI)
        self.assertIn("solar_os_expansion_attach(", TUI)
        self.assertIn("solar_os_expansion_detach(", TUI)
        self.assertIn("solar_os_expansion_category_name(", TUI)
        self.assertIn('"Devices"', TUI)
        self.assertIn('"Drivers"', TUI)

    def test_attach_form_uses_live_resource_choices_and_links_io_map(self):
        self.assertIn("solar_os_expansion_binding_pin_supported", TUI)
        self.assertIn("solar_os_resource_find_claim", TUI)
        self.assertIn("solar_os_bus_count_protocol", TUI)
        self.assertIn("solar_os_connector_pin_get_info", TUI)
        self.assertIn('solar_os_app_registry_find("io")', TUI)
        self.assertIn("SOLAR_OS_LAUNCH_CHILD_RETURN", TUI)
        self.assertIn('"choose resource"', TUI)
        self.assertIn("E manual", TUI)
        self.assertIn("P pin map/buses", TUI)

    def test_tui_does_not_manage_bus_lifecycle(self):
        self.assertNotIn("solar_os_bus_attach(", TUI)
        self.assertNotIn("solar_os_bus_detach(", TUI)
        self.assertNotIn("solar_os_bus_register(", TUI)
        self.assertNotIn("solar_os_bus_unregister(", TUI)

    def test_package_builds_tui_with_expansion_service(self):
        package_start = PACKAGES.index("[packages.service_expansion]")
        package_end = PACKAGES.index("\n[packages.", package_start + 1)
        package = PACKAGES[package_start:package_end]
        self.assertIn('"shell/solar_os_shell_expansion_tui.c"', package)


if __name__ == "__main__":
    unittest.main()
