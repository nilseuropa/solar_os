from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
WIREGUARD = (ROOT / "src/services/solar_os_wireguard.c").read_text(
    encoding="utf-8"
)
WIREGUARD_HEADER = (ROOT / "src/services/solar_os_wireguard.h").read_text(
    encoding="utf-8"
)
NETWORK_STATUS = (
    ROOT / "src/shell/solar_os_shell_network_status.c"
).read_text(encoding="utf-8")
NETWORK_TUI = (ROOT / "src/shell/solar_os_shell_network_tui.c").read_text(
    encoding="utf-8"
)
BOOT_SERVICES = (ROOT / "src/services/solar_os_boot_services.c").read_text(
    encoding="utf-8"
)
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(
    encoding="utf-8"
)


class WireGuardUplinkTest(unittest.TestCase):
    def test_runtime_uses_preferred_network_path(self):
        self.assertIn("solar_os_network_path_get_preferred", WIREGUARD)
        self.assertIn("SOLAR_OS_NETWORK_EVENT_PATHS_CHANGED", WIREGUARD)
        self.assertIn(".bind_netif = request->underlay_netif", WIREGUARD)
        self.assertIn("wireguard_service.bound_netif != uplink_netif", WIREGUARD)
        self.assertNotIn("solar_os_wifi_get_sta_netif", WIREGUARD)
        self.assertNotIn("IP_EVENT_STA_GOT_IP", WIREGUARD)
        self.assertNotIn("WIFI_EVENT_STA_DISCONNECTED", WIREGUARD)

    def test_wait_state_and_status_are_uplink_neutral(self):
        self.assertIn("SOLAR_OS_WIREGUARD_STATE_WAIT_UPLINK", WIREGUARD_HEADER)
        self.assertNotIn("SOLAR_OS_WIREGUARD_STATE_WAIT_WIFI", WIREGUARD_HEADER)
        self.assertIn('return "waiting for uplink"', WIREGUARD)
        self.assertIn("status->underlay", WIREGUARD)
        self.assertNotIn("waiting for Wi-Fi", WIREGUARD)

    def test_network_views_do_not_hardcode_wireguard_underlay(self):
        self.assertIn("wireguard.underlay", NETWORK_STATUS)
        self.assertIn("wireguard.underlay", NETWORK_TUI)
        self.assertNotIn("wireguard over wifi-sta", NETWORK_STATUS)
        self.assertNotIn("wireguard over wifi-sta", NETWORK_TUI)

    def test_package_and_boot_are_not_wifi_gated(self):
        network_start = PACKAGES.index("[packages.service_network]")
        network_end = PACKAGES.index("\n[packages.", network_start + 1)
        network_package = PACKAGES[network_start:network_end]
        wireguard_start = PACKAGES.index("[packages.service_wireguard]")
        wireguard_end = PACKAGES.index("\n[packages.", wireguard_start + 1)
        wireguard_package = PACKAGES[wireguard_start:wireguard_end]

        self.assertIn('"services/solar_os_lwip_route.c"', network_package)
        self.assertIn('depends = ["service_network"]', wireguard_package)
        self.assertNotIn('capabilities = ["wifi"]', wireguard_package)
        self.assertNotIn('"esp_wifi"', wireguard_package)

        init = BOOT_SERVICES.index("solar_os_wireguard_init()")
        guard = BOOT_SERVICES.rfind("#if", 0, init)
        self.assertIn("SOLAR_OS_PACKAGE_SERVICE_WIREGUARD", BOOT_SERVICES[guard:init])
        self.assertNotIn("SOLAR_OS_BOARD_CAP_WIFI", BOOT_SERVICES[guard:init])


if __name__ == "__main__":
    unittest.main()
