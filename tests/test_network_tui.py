from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMMAND = (ROOT / "src/shell/solar_os_shell_network_status.c").read_text(
    encoding="utf-8"
)
TUI = (ROOT / "src/shell/solar_os_shell_network_tui.c").read_text(
    encoding="utf-8"
)
NETWORK = (ROOT / "src/services/solar_os_network.c").read_text(encoding="utf-8")
NETWORK_HEADER = (ROOT / "src/services/solar_os_network.h").read_text(
    encoding="utf-8"
)
PPPD = (ROOT / "src/jobs/solar_os_pppd_job.c").read_text(encoding="utf-8")
SLIP = (ROOT / "src/jobs/solar_os_slip_job.c").read_text(encoding="utf-8")
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(
    encoding="utf-8"
)


class NetworkTuiTest(unittest.TestCase):
    def test_bare_command_launches_tui_and_status_remains_textual(self):
        start = COMMAND.index("void solar_os_shell_cmd_network(")
        command = COMMAND[start:]

        self.assertIn("if (argc == 1)", command)
        self.assertIn("solar_os_shell_launch_network_tui(ctx)", command)
        self.assertIn('strcmp(argv[1], "status") == 0', command)
        self.assertLess(
            command.index("solar_os_shell_launch_network_tui(ctx)"),
            command.index('strcmp(argv[1], "status") == 0'),
        )

    def test_tui_has_status_and_settings_tabs(self):
        self.assertIn('static const char status_label[] = " Status ";', TUI)
        self.assertIn('static const char settings_label[] = " Settings ";', TUI)
        self.assertIn("network_tui_build_status_rows", TUI)
        self.assertIn("network_tui_draw_settings", TUI)

    def test_settings_use_generic_network_service(self):
        self.assertIn("solar_os_network_path_set_priority", TUI)
        self.assertIn("solar_os_network_router_start()", TUI)
        self.assertIn("solar_os_network_router_stop()", TUI)
        self.assertNotIn("solar_os_wifi_", TUI)

    def test_settings_launch_transport_children_and_return(self):
        self.assertIn('"Configure"', TUI)
        self.assertIn('"  Wi-Fi..."', TUI)
        self.assertIn('"  Modems... (%u registered)"', TUI)
        self.assertIn("solar_os_shell_launch_wifi_tui_ex", TUI)
        self.assertIn("solar_os_shell_launch_modem_tui_ex", TUI)
        self.assertEqual(TUI.count("SOLAR_OS_LAUNCH_CHILD_RETURN"), 2)
        self.assertIn("SOLAR_OS_EVENT_RESUME", TUI)
        self.assertIn(".flags = SOLAR_OS_APP_FLAG_RESUMABLE", TUI)
        self.assertIn(".suspend = network_tui_suspend", TUI)
        self.assertIn(".resume = network_tui_resume", TUI)

    def test_status_explains_default_and_client_routing(self):
        self.assertIn('"default: %s (automatic)"', TUI)
        self.assertIn('"%s -> %s%s"', TUI)
        self.assertIn('"%s -> %s, NAT %u/%u"', TUI)
        self.assertIn('"wifi-ap: routing unavailable"', TUI)
        self.assertIn("Higher priority wins the default route.", TUI)
        self.assertIn("Downstream routing controls Wi-Fi AP clients.", TUI)

    def test_status_uses_role_aware_network_interface_inventory(self):
        self.assertIn("solar_os_network_interface_list", TUI)
        self.assertIn("SOLAR_OS_NETWORK_INTERFACE_ROLE_DOWNSTREAM", TUI)
        self.assertIn("SOLAR_OS_NETWORK_INTERFACE_ROLE_PEER", TUI)
        self.assertIn("solar_os_network_interface_role_name", TUI)
        self.assertIn("solar_os_network_interface_state_name", TUI)

        self.assertIn("solar_os_network_interface_publish", NETWORK_HEADER)
        self.assertIn("solar_os_network_interface_remove", NETWORK_HEADER)
        self.assertIn("solar_os_network_interface_list", NETWORK_HEADER)
        self.assertIn("network_interfaces", NETWORK)
        self.assertIn("SOLAR_OS_NETWORK_INTERFACE_ROLE_UPLINK", NETWORK)

        self.assertIn("pppd_publish_interface", PPPD)
        self.assertIn("solar_os_network_interface_remove", PPPD)
        self.assertIn("SOLAR_OS_NETWORK_INTERFACE_ROLE_DOWNSTREAM", PPPD)
        self.assertIn("SOLAR_OS_NETWORK_INTERFACE_ROLE_PEER", PPPD)
        self.assertIn("solar_os_network_interface_publish", SLIP)
        self.assertIn("solar_os_network_interface_remove", SLIP)

        slip_package = PACKAGES.split("[packages.job_slip]", 1)[1].split(
            "\n[packages.", 1
        )[0]
        self.assertIn('depends = ["service_uart", "service_network"]', slip_package)
        self.assertNotIn('capabilities = ["wifi"]', slip_package)

    def test_package_builds_tui_with_network_service(self):
        package_start = PACKAGES.index("[packages.service_network]")
        package_end = PACKAGES.index("\n[packages.", package_start + 1)
        package = PACKAGES[package_start:package_end]
        self.assertIn('"shell/solar_os_shell_network_tui.c"', package)


if __name__ == "__main__":
    unittest.main()
