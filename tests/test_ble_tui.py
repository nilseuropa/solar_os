from pathlib import Path
import unittest


ROOT = Path(__file__).resolve().parents[1]
COMMAND = (ROOT / "src/shell/solar_os_shell_hardware.c").read_text(
    encoding="utf-8"
)
TUI = (ROOT / "src/shell/solar_os_shell_ble_tui.c").read_text(
    encoding="utf-8"
)
PACKAGES = (ROOT / "packages/solar_os_packages.toml").read_text(
    encoding="utf-8"
)


class BleTuiTest(unittest.TestCase):
    def test_bare_command_launches_tui_and_status_remains_textual(self):
        start = COMMAND.index("void solar_os_shell_cmd_ble(")
        command = COMMAND[start:]
        self.assertIn("if (argc == 1)", command)
        self.assertIn("solar_os_shell_launch_ble_tui(ctx)", command)
        self.assertIn('strcmp(argv[1], "status") == 0', command)
        self.assertLess(
            command.index("solar_os_shell_launch_ble_tui(ctx)"),
            command.index('strcmp(argv[1], "status") == 0'),
        )
        launch_block = command.split('strcmp(argv[1], "status") == 0', 1)[0]
        self.assertNotIn("if (!current_boot_enabled)", launch_block)

    def test_tui_has_inspection_tabs_and_gatt_actions(self):
        for label in ('"Devices"', '"Services"', '"Chars"', '"Settings"'):
            self.assertIn(label, TUI)
        self.assertIn("solar_os_ble_scan", TUI)
        self.assertIn("solar_os_ble_peer_connect", TUI)
        self.assertIn("solar_os_ble_peer_services", TUI)
        self.assertIn("solar_os_ble_peer_characteristics", TUI)
        self.assertIn("solar_os_ble_peer_read", TUI)
        self.assertIn("solar_os_ble_peer_write", TUI)

    def test_scan_and_connect_show_progress_popups(self):
        begin_start = TUI.index("static void ble_tui_begin_connect_selected(void)")
        begin_end = TUI.index("static void ble_tui_run_connect(void)", begin_start)
        begin = TUI[begin_start:begin_end]
        run_end = TUI.index("static void ble_tui_disconnect(void)", begin_end)
        run = TUI[begin_end:run_end]
        self.assertIn("ble_tui.operation = BLE_TUI_OPERATION_CONNECT", begin)
        self.assertNotIn("solar_os_ble_peer_connect", begin)
        self.assertIn("solar_os_ble_peer_connect", run)
        self.assertLess(
            TUI.index("ble_tui.operation = BLE_TUI_OPERATION_CONNECT"),
            TUI.index("solar_os_ble_peer_connect"),
        )
        self.assertIn('"Scanning"', TUI)
        self.assertIn('"Connecting"', TUI)
        self.assertIn("Waiting for link and GATT discovery.", TUI)
        self.assertIn("ble_tui.timeout_ms / 1000U", TUI)
        self.assertIn("device->name", TUI)

    def test_ble_calls_start_on_tick_after_popup_frame(self):
        tick_start = TUI.index("if (event->type == SOLAR_OS_EVENT_TICK)")
        tick_end = TUI.index("if (event->type != SOLAR_OS_EVENT_CHAR)", tick_start)
        tick = TUI[tick_start:tick_end]
        self.assertIn("ble_tui_run_scan()", tick)
        self.assertIn("ble_tui_run_connect()", tick)
        self.assertIn("ble_tui_render()", tick)
        self.assertIn("BLE_TUI_PROGRESS_HOLD_MS", tick)

    def test_popups_use_common_renderer_without_local_restyling(self):
        popup_start = TUI.index("static void ble_tui_draw_popup(")
        popup_end = TUI.index("static void ble_tui_draw_connect_progress", popup_start)
        popup = TUI[popup_start:popup_end]
        self.assertIn("solar_os_tui_text_popup", popup)
        self.assertNotIn("SOLAR_OS_TUI_ATTR_INVERSE", popup)
        self.assertNotIn("solar_os_tui_write_cell", popup)

    def test_connect_failure_popup_reports_cause_until_dismissed(self):
        self.assertIn('ble_tui_set_popup("Connection failed", message)', TUI)
        self.assertIn("Cause: %s", TUI)
        self.assertIn("if (ble_tui.popup_active)", TUI)
        self.assertIn("ble_tui.popup_active = false", TUI)

    def test_footer_always_uses_key_mnemonics(self):
        self.assertIn("solar_os_tui_layout_compute(rows", TUI)
        self.assertIn("layout.help.row", TUI)
        self.assertIn("layout.status.row", TUI)

    def test_tui_exposes_ble_parameters(self):
        self.assertIn("solar_os_ble_keyboard_set_boot_setting", TUI)
        self.assertIn("solar_os_ble_keyboard_set_keepalive_enabled", TUI)
        self.assertIn("solar_os_ble_keyboard_set_layout", TUI)
        self.assertIn("solar_os_ble_keyboard_set_repeat", TUI)
        self.assertIn("BLE_TUI_SETTING_TIMEOUT", TUI)
        self.assertIn("BLE_TUI_SETTING_WRITE_MODE", TUI)
        self.assertIn("solar_os_ble_keyboard_start_pairing", TUI)
        self.assertIn("solar_os_ble_keyboard_forget", TUI)
        self.assertIn("BLE disabled this boot; use Settings", TUI)

    def test_tui_owns_and_closes_its_session(self):
        self.assertIn('solar_os_ble_session_create("ble.tui"', TUI)
        self.assertIn("solar_os_ble_peer_disconnect", TUI)
        self.assertIn("solar_os_ble_session_close", TUI)
        self.assertIn(".flags = SOLAR_OS_APP_FLAG_RESUMABLE", TUI)
        self.assertIn(".state_storage = SOLAR_OS_APP_STATE_EXTERNAL_PREFERRED", TUI)

    def test_package_builds_tui_with_ble_service(self):
        package_start = PACKAGES.index("[packages.service_ble]")
        package_end = PACKAGES.index("\n[packages.", package_start + 1)
        package = PACKAGES[package_start:package_end]
        self.assertIn('"shell/solar_os_shell_ble_tui.c"', package)


if __name__ == "__main__":
    unittest.main()
