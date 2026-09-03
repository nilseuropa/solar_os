from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RuntimeBoundaryTest(unittest.TestCase):
    def test_main_delegates_service_boot(self):
        main = (ROOT / "src/main.c").read_text(encoding="utf-8")
        boot = (ROOT / "src/services/solar_os_boot_services.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("solar_os_boot_services_init(millis_u32());", main)
        self.assertNotIn("static void init_peripherals", main)
        for init_call in (
            "solar_os_stream_init()",
            "solar_os_storage_init()",
            "solar_os_inbox_init()",
            "solar_os_chat_init()",
            "solar_os_ble_keyboard_init()",
        ):
            self.assertNotIn(init_call, main)
            self.assertIn(init_call, boot)

        ordered_calls = (
            "solar_os_stream_init()",
            "solar_os_port_init()",
            "solar_os_power_init()",
            "solar_os_storage_init()",
            "solar_os_identity_init()",
            "solar_os_inbox_init()",
            "solar_os_chat_init()",
        )
        positions = [boot.index(call) for call in ordered_calls]
        self.assertEqual(positions, sorted(positions))

    def test_io_uses_bus_capability_contract(self):
        io_app = (ROOT / "src/apps/solar_os_io.c").read_text(encoding="utf-8")
        buses = (ROOT / "src/services/solar_os_buses.c").read_text(encoding="utf-8")

        self.assertNotIn("driver/spi_master.h", io_app)
        self.assertNotIn("SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK", io_app)
        self.assertNotIn("SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK", io_app)
        self.assertIn("solar_os_bus_runtime_protocol_available", io_app)
        self.assertIn("solar_os_bus_runtime_endpoint_get", io_app)
        self.assertIn("SOLAR_OS_BOARD_RUNTIME_SPI_HOST_MASK", buses)
        self.assertIn("SOLAR_OS_BOARD_RUNTIME_UART_PORT_MASK", buses)

    def test_telnet_client_owns_a_scoped_wifi_latency_lease(self):
        telnetd = (ROOT / "src/jobs/solar_os_telnetd_job.c").read_text(
            encoding="utf-8"
        )
        wifi = (ROOT / "src/services/solar_os_wifi.c").read_text(
            encoding="utf-8"
        )

        accept_start = telnetd.index("static bool telnetd_accept_one(")
        accept_end = telnetd.index("static void telnetd_job_task(", accept_start)
        accept = telnetd[accept_start:accept_end]
        cleanup_start = telnetd.index("static bool telnetd_cleanup_client(")
        cleanup_end = telnetd.index("static void telnetd_reject_busy(", cleanup_start)
        cleanup = telnetd[cleanup_start:cleanup_end]

        self.assertIn("solar_os_wifi_latency_acquire", accept)
        self.assertIn("solar_os_wifi_latency_release", cleanup)
        self.assertIn(
            "wifi_connectionless_active || wifi_latency_owner[0] != '\\0'",
            wifi,
        )

    def test_wifi_repeater_is_l2_forwarding_not_nat_alias(self):
        wifi = (ROOT / "src/services/solar_os_wifi.c").read_text(
            encoding="utf-8"
        )
        repeater = (ROOT / "src/services/solar_os_wifi_repeater.c").read_text(
            encoding="utf-8"
        )
        routes = (ROOT / "src/services/solar_os_lwip_route.c").read_text(
            encoding="utf-8"
        )
        packages = (ROOT / "packages/solar_os_packages.toml").read_text(
            encoding="utf-8"
        )
        shell = (ROOT / "src/shell/solar_os_shell_network.c").read_text(
            encoding="utf-8"
        )
        completion = (ROOT / "src/apps/solar_os_shell.c").read_text(
            encoding="utf-8"
        )
        descriptor = (ROOT / "src/apps/solar_os_script_api.inc").read_text(
            encoding="utf-8"
        )

        start = wifi.index("esp_err_t solar_os_wifi_repeater_start(")
        end = wifi.index("esp_err_t solar_os_wifi_repeater_stop(", start)
        start_function = wifi[start:end]
        self.assertIn("if (solar_os_wifi_repeater_is_enabled())", start_function)
        self.assertIn("solar_os_wifi_connect_saved()", start_function)
        self.assertIn("wifi_ap_start_config(repeater_profile.ssid", start_function)
        self.assertIn("solar_os_wifi_repeater_enable(", start_function)
        self.assertLess(
            start_function.index("wifi_ap_start_config(repeater_profile.ssid"),
            start_function.index("solar_os_wifi_repeater_enable("),
        )
        self.assertIn("wifi_repeater_starting", start_function)
        self.assertNotIn("solar_os_wifi_nat_set(true)", start_function)
        self.assertIn("wifi_find_profile_index_locked(wifi_ssid)", start_function)
        self.assertIn('repeater_profile.password[0] == \'\\0\' ? "open" : "wpa2"', start_function)
        self.assertIn("false);", start_function)
        self.assertIn("request->ap->input = repeater_ap_input", repeater)
        self.assertIn("request->sta->input = repeater_sta_input", repeater)
        self.assertIn("repeater.ap->input == repeater_ap_input", repeater)
        self.assertIn("repeater.sta->input == repeater_sta_input", repeater)
        self.assertIn("esp_netif_dhcps_stop", repeater)
        self.assertIn("repeater_send_proxy_arp", repeater)
        self.assertIn("SOLAR_OS_WIFI_REPEATER_CLIENT_MAX", repeater)
        self.assertNotIn("esp_netif_napt_enable", repeater)
        self.assertIn("solar_os_wifi_repeater_route(dest)", routes)
        self.assertIn("solar_os_wifi_repeater_upstream_route()", routes)
        self.assertIn("wifi_repeater_schedule_reconnect()", wifi)
        self.assertIn('"services/solar_os_wifi_repeater.c"', packages)
        self.assertIn('strcmp(argv[1], "repeater")', shell)
        self.assertIn('"repeater",', completion)
        self.assertIn(
            "SHELL_COMPLETION_STATIC(path_wifi_repeater, wifi_repeater_subcommands)",
            completion,
        )
        self.assertIn(
            'static const char * const wifi_repeater_subcommands[] = {"on", "off"};',
            completion,
        )
        self.assertNotIn("path_wifi_repeater_on_auth", completion)
        for method in ("repeater_start", "repeater_stop"):
            self.assertIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(wifi, {method}, {method});",
                descriptor,
            )

    def test_radio_link_repeater_is_one_hop_and_bounded(self):
        link_header = (ROOT / "src/services/solar_os_link.h").read_text(
            encoding="utf-8"
        )
        repeater_header = (
            ROOT / "src/services/solar_os_link_repeater.h"
        ).read_text(encoding="utf-8")
        repeater = (
            ROOT / "src/services/solar_os_link_repeater.c"
        ).read_text(encoding="utf-8")
        radio_link = (
            ROOT / "src/jobs/solar_os_radio_link_job.c"
        ).read_text(encoding="utf-8")
        completion = (ROOT / "src/apps/solar_os_shell.c").read_text(
            encoding="utf-8"
        )
        packages = (ROOT / "packages/solar_os_packages.toml").read_text(
            encoding="utf-8"
        )

        self.assertIn("SOLAR_OS_LINK_FLAG_RELAYED", link_header)
        self.assertIn("SOLAR_OS_LINK_REPEATER_PENDING_MAX 4U", repeater_header)
        self.assertIn("SOLAR_OS_LINK_REPEATER_CACHE_MAX 16U", repeater_header)
        self.assertIn("SOLAR_OS_LINK_REPEATER_DELAY_MIN_MS 80U", repeater_header)
        self.assertIn("message->flags & SOLAR_OS_LINK_FLAG_RELAYED", repeater)
        self.assertIn("repeater_cancel_acknowledged", repeater)
        self.assertIn("message.flags |= SOLAR_OS_LINK_FLAG_RELAYED", radio_link)
        self.assertIn('static const char repeater_prefix[] = "repeater=";', radio_link)
        self.assertIn('"repeater=off"', completion)
        self.assertIn('"repeater=on"', completion)
        self.assertIn(
            "SHELL_COMPLETION_STATIC(path_job_start_radio_link_option_2,",
            completion,
        )
        self.assertIn('"services/solar_os_link_repeater.c"', packages)

    def test_boot_coordinator_is_packaged(self):
        packages = (ROOT / "packages/solar_os_packages.toml").read_text(
            encoding="utf-8"
        )
        self.assertIn('"services/solar_os_boot_services.c"', packages)

    def test_script_and_ble_policies_are_delegated(self):
        python = (ROOT / "src/apps/solar_os_python.c").read_text(encoding="utf-8")
        lua = (ROOT / "src/apps/solar_os_lua.c").read_text(encoding="utf-8")
        ble = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text(
            encoding="utf-8"
        )

        for interpreter in (python, lua):
            self.assertIn("solar_os_script_wait_for_stop", interpreter)
            self.assertNotIn("xTaskGetTickCount() - start) < pdMS_TO_TICKS", interpreter)

        self.assertIn("solar_os_ble_keyboard_scan_candidate_should_replace", ble)
        self.assertNotIn("hid_keycode_to_char", ble)
        self.assertIn(
            "return solar_os_input_set_keyboard_layout(\n"
            "        (solar_os_input_keyboard_layout_t)value);",
            ble,
        )

    def test_audio_stream_direction_and_shell_capabilities(self):
        audio = (ROOT / "src/services/solar_os_audio.c").read_text(
            encoding="utf-8"
        )
        registry = (ROOT / "src/apps/solar_os_app_registry.c").read_text(
            encoding="utf-8"
        )

        playback_start = audio.index("esp_err_t solar_os_audio_stream_open(")
        playback_end = audio.index("esp_err_t solar_os_audio_stream_write(")
        playback = audio[playback_start:playback_end]
        capture_start = audio.index(
            "esp_err_t solar_os_audio_input_stream_open("
        )
        capture_end = audio.index("esp_err_t solar_os_audio_input_stream_read(")
        capture = audio[capture_start:capture_end]
        self.assertNotIn("solar_os_audio_backend_has_input()", playback)
        self.assertIn("solar_os_audio_backend_has_input()", capture)

        for app_name in ("aplay", "arecord"):
            entry = next(
                line
                for line in registry.splitlines()
                if f'APP_ENTRY("{app_name}"' in line
            )
            self.assertIn("SOLAR_OS_APP_CAP_DISPLAY", entry)
            self.assertIn("SOLAR_OS_APP_CAP_PORT", entry)

        arecord = next(
            line
            for line in registry.splitlines()
            if 'APP_ENTRY("arecord"' in line
        )
        self.assertIn("[-d seconds] [-i capture-stream] <file.wav>", arecord)
        self.assertTrue(arecord.endswith(", 2, 6),"))

    def test_playground_registry_usage_lists_every_subcommand(self):
        registry = (ROOT / "src/apps/solar_os_app_registry.c").read_text(
            encoding="utf-8"
        )
        entry = next(
            line
            for line in registry.splitlines()
            if 'APP_ENTRY("playground"' in line
        )
        for command in (
            "search",
            "install",
            "run",
            "delete",
            "refresh",
            "reload",
            "source",
            "storage",
        ):
            self.assertIn(command, entry)

    def test_sessions_restore_apps_without_resume_renderers(self):
        sessions = (ROOT / "src/services/solar_os_sessions.c").read_text(
            encoding="utf-8"
        )
        gfx = (ROOT / "src/services/solar_os_gfx.c").read_text(encoding="utf-8")

        self.assertGreaterEqual(
            sessions.count("session_capture_graphics_snapshot(session);"), 2
        )
        self.assertGreaterEqual(
            sessions.count("session_restore_graphics_snapshot(session);"), 4
        )
        self.assertIn("if (!was_started || !session->graphics_active)", sessions)
        self.assertIn("solar_os_gfx_snapshot_capture", gfx)
        self.assertIn("solar_os_gfx_snapshot_restore", gfx)
        self.assertIn("u8g2_GetBufferPtr(gfx->u8g2)", gfx)
        self.assertIn("surface->data", gfx)

    def test_session_switch_clear_is_not_presented(self):
        main = (ROOT / "src/main.c").read_text(encoding="utf-8")
        start = main.index("static void session_overlay_requested(")
        end = main.index("static void dispatch_app_resume(", start)
        overlay_request = main[start:end]

        self.assertIn("u8g2_ClearBuffer(display_u8g2);", overlay_request)
        self.assertNotIn(
            "solar_os_display_present(display_u8g2", overlay_request
        )

    def test_gameboy_presents_clean_first_resume_frame(self):
        gameboy = (ROOT / "src/apps/solar_os_gameboy_presenter.c").read_text(
            encoding="utf-8"
        )
        presenter = (ROOT / "src/services/solar_os_frame_presenter.c").read_text(
            encoding="utf-8"
        )
        tft = (ROOT / "src/drivers/tft_ili9341.c").read_text(encoding="utf-8")

        self.assertIn(".clear_background_on_resume = true", gameboy)
        self.assertIn(".background_index = 0U", gameboy)
        self.assertIn("presenter->clear_background_pending", presenter)
        self.assertIn(".clear_background = presenter->clear_background_pending", presenter)
        self.assertIn("if (lines->frame->clear_background)", tft)
        self.assertIn("display->config.width - 1U", tft)
        self.assertIn("display->config.height - 1U", tft)

    def test_foreground_apps_use_one_class_lifecycle(self):
        sources = list((ROOT / "src/apps").glob("*.c"))
        sources += list((ROOT / "src/shell").glob("*.c"))
        descriptors = []
        for path in sources:
            text = path.read_text(encoding="utf-8")
            for match in re.finditer(
                r"(?:static\s+)?const\s+solar_os_app_t\s+\w+\s*=\s*\{(.*?)\n\};",
                text,
                re.DOTALL,
            ):
                descriptors.append((path, match.group(1)))

        self.assertGreaterEqual(len(descriptors), 45)
        for path, body in descriptors:
            self.assertRegex(
                body,
                r"\.app_class\s*=\s*SOLAR_OS_APP_CLASS_(?:COMMAND|TUI|GUI)",
                str(path.relative_to(ROOT)),
            )

        lifecycle_sources = "\n".join(
            path.read_text(encoding="utf-8") for path in sources
        )
        for legacy_api in (
            "solar_os_context_request_exit(",
            "solar_os_context_request_exit_result(",
            "solar_os_context_request_terminal_preserve(",
            "solar_os_context_set_status_message(",
        ):
            self.assertNotIn(legacy_api, lifecycle_sources)
        self.assertNotIn("error_only", lifecycle_sources)

    def test_command_output_is_separate_from_private_screen_state(self):
        sessions = (ROOT / "src/services/solar_os_sessions.c").read_text(
            encoding="utf-8"
        )
        terminal = (ROOT / "src/services/solar_os_terminal.c").read_text(
            encoding="utf-8"
        )
        python = (ROOT / "src/apps/solar_os_python.c").read_text(
            encoding="utf-8"
        )
        lua = (ROOT / "src/apps/solar_os_lua.c").read_text(encoding="utf-8")
        files = (ROOT / "src/apps/solar_os_files.c").read_text(encoding="utf-8")
        shell_io = (ROOT / "src/shell/solar_os_shell_io.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("session_write_text_output", sessions)
        self.assertIn("session_transfer_exit_result", sessions)
        self.assertIn(
            "session->shell_session =\n"
            "                solar_os_context_shell_session(session_state.ctx);",
            sessions,
        )
        self.assertIn(
            "session->terminal != NULL && session->terminal == shell->terminal",
            sessions,
        )
        self.assertIn("session_text_output_ends_with", sessions)
        self.assertEqual(
            sessions.count(
                "solar_os_app_stop(session->app, session_state.ctx);\n"
                "            session_transfer_exit_result(session);\n"
                "            session_dispose_unstarted(session);"
            ),
            2,
        )
        self.assertNotIn("solar_os_terminal_append_text", sessions)
        self.assertNotIn("solar_os_terminal_append_text", terminal)
        self.assertIn("solar_os_shell_io_capture_output", shell_io)
        self.assertIn("SOLAR_OS_APP_CLASS_COMMAND", shell_io)
        clear = shell_io.split("esp_err_t solar_os_shell_io_clear(", 1)[1].split(
            "esp_err_t solar_os_shell_io_newline(", 1
        )[0]
        self.assertNotIn("shell_io_mirror", clear)
        for interpreter in (python, lua):
            self.assertRegex(
                interpreter,
                r"solar_os_context_set_app_class\(\s*ctx,\s*"
                r"\w+\s*\?\s*SOLAR_OS_APP_CLASS_TUI\s*:\s*"
                r"SOLAR_OS_APP_CLASS_COMMAND\s*\)",
            )
            self.assertIn("solar_os_context_finish", interpreter)
        self.assertIn("mp_obj_exception_get_value(exception)", python)
        self.assertIn("luaL_optinteger(L, 1, 0)", lua)
        self.assertNotIn("SOLAR_OS_APP_CLASS_COMMAND", files)


if __name__ == "__main__":
    unittest.main()
