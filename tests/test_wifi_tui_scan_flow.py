from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
TUI = (ROOT / "src/shell/solar_os_shell_wifi_tui.c").read_text(encoding="utf-8")
WIFI = (ROOT / "src/services/solar_os_wifi.c").read_text(encoding="utf-8")


def function(source: str, signature: str, next_signature: str) -> str:
    start = source.index(signature)
    end = source.index(next_signature, start)
    return source[start:end]


class WifiTuiScanFlowTest(unittest.TestCase):
    def test_router_policy_is_not_owned_by_the_wifi_tui(self):
        self.assertNotIn("WIFI_TUI_SHARE", TUI)
        self.assertNotIn("internet share", TUI)
        self.assertNotIn("solar_os_network_router_", TUI)

    def test_repeater_is_visible_and_toggles_the_service(self):
        current_value = function(
            TUI,
            "static void wifi_tui_current_value(",
            "static size_t wifi_tui_scan_visible_rows(",
        )
        apply_selected = function(
            TUI,
            "static void wifi_tui_apply_selected(void)",
            "static esp_err_t wifi_tui_start(",
        )

        self.assertIn('[WIFI_TUI_REPEATER] = {.label = "repeater"}', TUI)
        self.assertIn("case WIFI_TUI_REPEATER:", current_value)
        self.assertIn("wifi_tui_repeater_value(status", current_value)
        self.assertIn("status.repeater_enabled ?", apply_selected)
        self.assertIn("solar_os_wifi_repeater_stop()", apply_selected)
        self.assertIn("solar_os_wifi_repeater_start()", apply_selected)
        self.assertIn('wifi_tui_set_status("no saved upstream")', apply_selected)

    def test_saved_connect_uses_only_the_most_recent_profile(self):
        select_profile = function(
            WIFI,
            "static esp_err_t wifi_select_saved_profile(wifi_profile_t *selected)",
            "static void wifi_clear_saved_ap_config_locked(void)",
        )
        self.assertIn("*selected = wifi_profiles[0];", select_profile)
        self.assertNotIn("esp_wifi_scan_start", select_profile)

    def test_popup_is_rendered_before_nonblocking_scan_starts(self):
        open_scan = function(
            TUI,
            "static void wifi_tui_open_scan(void)",
            "static bool wifi_tui_poll_scan(void)",
        )
        self.assertLess(
            open_scan.index("wifi_tui_render();"),
            open_scan.index("solar_os_wifi_scan_start_async();"),
        )

    def test_scan_popup_remains_until_async_results_are_ready(self):
        poll_scan = function(
            TUI,
            "static bool wifi_tui_poll_scan(void)",
            "static void wifi_tui_open_station_password(void)",
        )
        self.assertIn("solar_os_wifi_scan_results(", poll_scan)
        self.assertIn("if (err == ESP_ERR_NOT_FINISHED)", poll_scan)
        self.assertIn("wifi_tui.view = WIFI_TUI_VIEW_SCAN;", poll_scan)

    def test_service_scan_is_nonblocking(self):
        start_scan = function(
            WIFI,
            "esp_err_t solar_os_wifi_scan_start_async(void)",
            "esp_err_t solar_os_wifi_scan_cancel_async(void)",
        )
        self.assertIn("esp_wifi_scan_start(NULL, false)", start_scan)


if __name__ == "__main__":
    unittest.main()
