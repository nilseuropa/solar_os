from pathlib import Path
import re
import unittest


ROOT = Path(__file__).resolve().parents[1]


class RuntimeBoundaryTest(unittest.TestCase):
    def test_inactive_expansion_driver_registries_use_external_bss(self):
        declarations = {
            "src/services/solar_os_ssd1306.c":
                "static EXT_RAM_BSS_ATTR solar_os_ssd1306_device_t devices",
            "src/services/solar_os_pcd8544.c":
                "static EXT_RAM_BSS_ATTR solar_os_pcd8544_device_t devices",
            "src/services/solar_os_analog_joystick.c":
                "static EXT_RAM_BSS_ATTR solar_os_analog_joystick_device_t",
            "src/services/solar_os_cardkb.c":
                "static EXT_RAM_BSS_ATTR solar_os_cardkb_device_t cardkb_devices",
            "src/services/solar_os_gpio_keys.c":
                "static EXT_RAM_BSS_ATTR solar_os_gpio_keys_device_t devices",
        }
        for key, declaration in declarations.items():
            relative_path = key.split("#", 1)[0]
            source = (ROOT / relative_path).read_text(encoding="utf-8")
            self.assertIn(declaration, source, key)

    def test_psram_safe_core_metadata_uses_external_bss(self):
        declarations = {
            "src/solar_os_jobs.c":
                "static EXT_RAM_BSS_ATTR solar_os_job_runtime_t job_runtimes",
            "src/services/solar_os_sessions.c":
                "static EXT_RAM_BSS_ATTR solar_os_session_state_t session_state",
            "src/apps/solar_os_app_registry.c":
                "static EXT_RAM_BSS_ATTR char app_owners",
            "src/apps/solar_os_shell.c":
                "static EXT_RAM_BSS_ATTR shell_completion_index_t shell_completion_index",
            "src/main.c":
                "static EXT_RAM_BSS_ATTR solar_os_context_t os_ctx",
        }
        for relative_path, declaration in declarations.items():
            source = (ROOT / relative_path).read_text(encoding="utf-8")
            self.assertIn(declaration, source, relative_path)

    def test_hardware_facing_core_registries_stay_internal(self):
        declarations = {
            "src/services/solar_os_buses.c":
                "static solar_os_bus_info_t buses",
            "src/services/solar_os_port.c":
                "static solar_os_port_entry_t ports",
            "src/jobs/solar_os_telnetd_job.c":
                "static telnetd_job_state_t telnetd_job",
        }
        for relative_path, declaration in declarations.items():
            source = (ROOT / relative_path).read_text(encoding="utf-8")
            self.assertIn(declaration, source, relative_path)

    def test_task_context_service_metadata_uses_external_bss(self):
        declarations = {
            "src/services/solar_os_network.c":
                "static EXT_RAM_BSS_ATTR network_path_entry_t",
            "src/services/solar_os_http_server.c":
                "static EXT_RAM_BSS_ATTR http_route_slot_t route_slots",
            "src/services/solar_os_ramfs.c":
                "static EXT_RAM_BSS_ATTR ramfs_mount_t mounts",
            "src/services/solar_os_modem.c":
                "static EXT_RAM_BSS_ATTR modem_device_t modem_devices",
            "src/services/solar_os_gnss.c":
                "static EXT_RAM_BSS_ATTR gnss_device_t gnss_devices",
            "src/services/solar_os_radio.c":
                "static EXT_RAM_BSS_ATTR radio_device_t radio_devices",
            "src/services/solar_os_chat.c":
                "static EXT_RAM_BSS_ATTR solar_os_chat_store_state_t chat",
            "src/services/solar_os_chat_transport_gateway.c":
                "static EXT_RAM_BSS_ATTR solar_os_chat_state_data_t chat_state",
        }
        for relative_path, declaration in declarations.items():
            source = (ROOT / relative_path).read_text(encoding="utf-8")
            self.assertIn(declaration, source, relative_path)

    def test_expansion_registry_prefers_external_memory(self):
        source = (ROOT / "src/services/solar_os_expansion.c").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("SOLAR_OS_EXPANSION_DEVICE_MAX", source)
        self.assertIn("solar_os_memory_calloc(", source)
        self.assertIn("SOLAR_OS_MEMORY_EXTERNAL_PREFERRED", source)
        self.assertIn("static StaticSemaphore_t devices_mutex_storage;", source)
        self.assertNotIn("portENTER_CRITICAL(&devices_lock)", source)

    def test_t_lora_devices_preserve_registered_resources_on_detach(self):
        keyboard = (ROOT / "src/services/solar_os_tca8418.c").read_text(
            encoding="utf-8"
        )
        radio = (ROOT / "src/services/solar_os_sx1262.c").read_text(
            encoding="utf-8"
        )

        clear_keyboard = keyboard.split("static void clear_device", 1)[1].split(
            "esp_err_t solar_os_tca8418_attach", 1
        )[0]
        self.assertIn("pwm_port_stop", clear_keyboard)
        self.assertIn("backlight_active", clear_keyboard)

        detach_radio = radio.split("esp_err_t solar_os_sx1262_detach", 1)[1]
        self.assertIn(
            "ESP_RETURN_ON_ERROR(solar_os_radio_unregister(name)", detach_radio
        )
        self.assertLess(
            detach_radio.index("solar_os_radio_unregister(name)"),
            detach_radio.index("clear_device(device)"),
        )

    def test_gt911_uses_only_claimed_i2c_addresses(self):
        driver = (ROOT / "src/drivers/gt911.c").read_text(encoding="utf-8")
        init = driver.split("esp_err_t gt911_init", 1)[1].split(
            "esp_err_t gt911_read", 1
        )[0]
        manifest = (ROOT / "boards/manifests/t_deck_plus.toml").read_text(
            encoding="utf-8"
        )

        self.assertIn("device_address = address;", init)
        self.assertIn("device_address = alternate_address;", init)
        self.assertNotIn("device_address = GT911_ALTERNATE_ADDRESS", init)
        self.assertIn("addr = 0x5d, alt_addr = 0x14", manifest)

    def test_mgc3130_is_a_buttonless_hover_pointer(self):
        source = (ROOT / "src/services/solar_os_mgc3130.c").read_text(
            encoding="utf-8"
        )
        registration = source.split(
            "const uint32_t capabilities =", 1
        )[1].split("if (ret != ESP_OK)", 1)[0]
        pointer = source.split("static void publish_pointer", 1)[1].split(
            "static uint32_t gesture_flags", 1
        )[0]

        self.assertIn("SOLAR_OS_INPUT_CAP_POINTER_ABSOLUTE", registration)
        self.assertNotIn("SOLAR_OS_INPUT_CAP_POINTER_BUTTONS", registration)
        self.assertIn("SOLAR_OS_INPUT_SOURCE_GESTURE", registration)
        self.assertNotIn("SOLAR_OS_INPUT_SOURCE_TOUCH", registration)
        self.assertIn(".action = SOLAR_OS_INPUT_POINTER_MOVE", pointer)
        self.assertIn(".buttons = 0", pointer)

    def test_graffiti_tracks_one_pointer_source_per_stroke(self):
        source = (ROOT / "src/jobs/solar_os_graffiti_job.c").read_text(
            encoding="utf-8"
        )
        pointer_filter = source.split(
            "static bool graffiti_job_filter", 1
        )[1].split("static void graffiti_job_worker", 1)[0]

        self.assertIn("state->pointer_source = event->source", pointer_filter)
        self.assertIn("event->source != state->pointer_source", pointer_filter)

    def test_system_key_can_emit_a_board_defined_short_press_input(self):
        main = (ROOT / "src/main.c").read_text(encoding="utf-8")
        short_press = main.split("static void handle_key_short_press", 1)[1].split(
            "static void key_button_init", 1
        )[0]

        self.assertIn("SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY", short_press)
        self.assertIn("solar_os_input_write_char(", short_press)
        self.assertIn("return;", short_press)

    def test_expansion_drivers_declare_categories(self):
        descriptor_sources = []
        for path in (ROOT / "src/services").glob("*.c"):
            source = path.read_text(encoding="utf-8")
            if "const solar_os_expansion_driver_t" in source:
                descriptor_sources.append((path, source))
        self.assertTrue(descriptor_sources)
        for path, source in descriptor_sources:
            descriptor_count = len(re.findall(
                r"(?:static )?const solar_os_expansion_driver_t\s+\w+\s*=\s*\{",
                source,
            ))
            if descriptor_count == 0:
                continue
            category_count = source.count(
                ".category = SOLAR_OS_EXPANSION_CATEGORY_"
            )
            self.assertEqual(category_count, descriptor_count, path.name)

    def test_expansion_driver_command_orders_explicit_categories(self):
        shell = (ROOT / "src/shell/solar_os_shell_expansion.c").read_text(
            encoding="utf-8"
        )
        self.assertNotIn('"CATEGORY"', shell)
        self.assertIn("solar_os_shell_io_printf_bold(", shell)
        self.assertIn('"  %-*s %-5s %-6s %s\\n"', shell)
        self.assertIn("expansion_next_driver_in_category", shell)
        self.assertIn("strcmp(driver.name, next->name) < 0", shell)
        self.assertIn(
            "solar_os_expansion_category_name(category)",
            shell,
        )

    def test_rotary_encoder_is_interrupt_driven_without_iram_handler(self):
        rotary = (ROOT / "src/services/solar_os_rotary_encoder.c").read_text(
            encoding="utf-8"
        )
        self.assertIn("GPIO_INTR_ANYEDGE", rotary)
        self.assertEqual(rotary.count("gpio_isr_handler_add("), 2)
        self.assertIn("xQueueSendFromISR", rotary)
        self.assertIn("xQueueReceive", rotary)
        self.assertNotIn("ROTARY_POLL_MS", rotary)
        self.assertNotIn("IRAM_ATTR rotary_gpio_isr", rotary)

    def test_telnet_uses_an_external_listener_and_internal_shell_stack(self):
        telnetd = (ROOT / "src/jobs/solar_os_telnetd_job.c").read_text(
            encoding="utf-8"
        )
        port_shell = (ROOT / "src/services/solar_os_port_shell.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("solar_os_task_create_pinned_external(telnetd_job_task", telnetd)
        self.assertIn("solar_os_task_delete_external(NULL);", telnetd)
        self.assertIn(".worker_stack_external = true,", telnetd)
        self.assertIn("#define PORT_SHELL_TASK_STACK 16384", port_shell)

    def test_python_stack_budget_and_task_failure_diagnostics(self):
        python = (ROOT / "src/apps/solar_os_python.c").read_text(
            encoding="utf-8"
        )
        task_header = (ROOT / "src/solar_os_task.h").read_text(
            encoding="utf-8"
        )
        task_source = (ROOT / "src/solar_os_task.c").read_text(
            encoding="utf-8"
        )
        shell = (ROOT / "src/shell/solar_os_shell_system.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("#define PYTHON_TASK_STACK (12U * 1024U)", python)
        self.assertIn("stack_min_free=%u", python)
        self.assertIn("last_failure_internal_largest_block_bytes", task_header)
        self.assertIn("heap_caps_get_largest_free_block(internal_caps)", task_source)
        self.assertIn('"  at failure: internal free %s max %s; "', shell)

    def test_audio_tone_worker_has_stack_for_default_device_playback(self):
        audio = (ROOT / "src/services/solar_os_audio.c").read_text(
            encoding="utf-8"
        )

        self.assertIn("#define AUDIO_TONE_WORKER_STACK 8192U", audio)

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

    def test_network_router_uses_paths_without_becoming_wifi_policy(self):
        network_header = (ROOT / "src/services/solar_os_network.h").read_text(
            encoding="utf-8"
        )
        network = (ROOT / "src/services/solar_os_network.c").read_text(
            encoding="utf-8"
        )
        routes = (ROOT / "src/services/solar_os_lwip_route.c").read_text(
            encoding="utf-8"
        )
        wifi = (ROOT / "src/services/solar_os_wifi.c").read_text(
            encoding="utf-8"
        )
        ppp = (ROOT / "src/services/solar_os_ppp.c").read_text(
            encoding="utf-8"
        )
        ppp_header = (ROOT / "src/services/solar_os_ppp.h").read_text(
            encoding="utf-8"
        )
        sim7670 = (ROOT / "src/services/solar_os_sim7670.c").read_text(
            encoding="utf-8"
        )
        wifi_shell = (ROOT / "src/shell/solar_os_shell_network.c").read_text(
            encoding="utf-8"
        )
        network_shell = (
            ROOT / "src/shell/solar_os_shell_network_status.c"
        ).read_text(encoding="utf-8")
        completion = (ROOT / "src/apps/solar_os_shell.c").read_text(
            encoding="utf-8"
        )
        descriptor = (ROOT / "src/apps/solar_os_script_api.inc").read_text(
            encoding="utf-8"
        )
        packages = (ROOT / "packages/solar_os_packages.toml").read_text(
            encoding="utf-8"
        )

        self.assertIn("esp_netif_dns_info_t dns;", network_header)
        self.assertIn("solar_os_network_path_get_preferred", network_header)
        self.assertIn("solar_os_network_path_list", network_header)
        self.assertIn("solar_os_network_path_set_priority", network_header)
        self.assertIn("solar_os_network_path_set_connecting", network_header)
        self.assertIn("solar_os_network_interface_publish", network_header)
        self.assertIn("solar_os_network_interface_list", network_header)
        self.assertIn("solar_os_network_router_provider_t", network_header)
        self.assertIn("candidate->info.route_priority >", network)
        self.assertIn("esp_netif_get_default_netif()", network)
        self.assertIn("esp_netif_get_dns_info(netif", network)
        self.assertIn("entry->info.dns = dns;", network)
        self.assertIn("esp_netif_set_route_prio", network)
        self.assertIn("NETWORK_NVS_PRIORITIES_KEY", network)
        self.assertIn("solar_os_network_lwip_preferred()", routes)

        apply_start = wifi.index("static esp_err_t wifi_apply_nat(void)")
        apply_end = wifi.index(
            "static esp_err_t wifi_update_ap_dns_from_path(",
            apply_start,
        )
        apply_nat = wifi[apply_start:apply_end]
        self.assertIn("solar_os_network_path_get_preferred(&path)", apply_nat)
        self.assertIn("wifi_update_ap_dns_from_path(&path)", apply_nat)
        self.assertNotIn("wifi_sta_enabled", apply_nat)
        self.assertNotIn("wifi_connected", apply_nat)
        self.assertNotIn("wifi_has_ip", apply_nat)

        router_start = wifi.rindex("static esp_err_t wifi_router_start(")
        router_end = wifi.index("static esp_err_t wifi_router_stop(", router_start)
        router = wifi[router_start:router_end]
        self.assertIn("wifi_ap_start_config(NULL, NULL, NULL, false)", router)
        self.assertIn("solar_os_wifi_nat_set(true)", router)
        self.assertLess(
            router.index("wifi_ap_start_config(NULL, NULL, NULL, false)"),
            router.index("solar_os_wifi_nat_set(true)"),
        )
        self.assertIn("solar_os_network_router_register(&router_provider)", wifi)
        self.assertIn("solar_os_network_path_register(\"wifi-sta\"", wifi)
        self.assertIn("SOLAR_OS_NETWORK_EVENT_PATHS_CHANGED", wifi)
        self.assertIn("solar_os_ppp_netif_binding_t", ppp_header)
        self.assertIn("solar_os_ppp_start", ppp_header)
        self.assertIn("SOLAR_OS_PPP_MODE_PASSIVE", ppp_header)
        self.assertNotIn("route_priority", ppp_header)
        self.assertNotIn("solar_os_network", ppp)
        self.assertIn("solar_os_network_path_register(device->name", sim7670)

        lwip_route = (ROOT / "src/services/solar_os_lwip_route.c").read_text(
            encoding="utf-8"
        )
        connected_fallback = lwip_route.index("if (src != NULL) {\n        return NULL;")
        destination_policy = lwip_route.index("if (dest != NULL && route_state.netif")
        self.assertLess(connected_fallback, destination_policy)
        self.assertIn("solar_os_network_path_set_ready(netif, ready)", sim7670)

        self.assertNotIn('strcmp(argv[1], "share")', wifi_shell)
        self.assertIn('strcmp(argv[1], "router")', network_shell)
        self.assertIn("solar_os_network_router_start()", network_shell)
        self.assertIn("solar_os_network_router_stop()", network_shell)
        self.assertIn(
            "SHELL_COMPLETION_STATIC(path_network_router, network_router_subcommands)",
            completion,
        )
        self.assertNotIn("path_wifi_share", completion)
        for method in ("router_start", "router_stop"):
            self.assertIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(net, {method}, {method});",
                descriptor,
            )
            self.assertNotIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(wifi, {method}, {method});",
                descriptor,
            )
        self.assertIn("[packages.service_network]", packages)
        service_ppp = packages.split("[packages.service_ppp]", 1)[1].split(
            "\n[packages.", 1
        )[0]
        sim7670_package = packages.split("[packages.sim7670]", 1)[1].split(
            "\n[packages.", 1
        )[0]
        self.assertNotIn("service_network", service_ppp)
        self.assertIn('"service_network"', sim7670_package)
        self.assertNotIn("service_uplink", packages)

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

    def test_ble_reconnect_is_scan_gated_to_the_remembered_peer(self):
        ble = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text(
            encoding="utf-8"
        )
        reconnect_start = ble.index("static void reconnect_task(")
        reconnect_end = ble.index("static void schedule_reconnect(", reconnect_start)
        reconnect = ble[reconnect_start:reconnect_end]
        candidate_start = ble.index("static void consider_candidate(")
        candidate_end = ble.index(
            "static bool key_in_report(", candidate_start
        )
        candidate = ble[candidate_start:candidate_end]

        self.assertIn(
            "scan_and_open_keyboard(BLE_KEYBOARD_SCAN_RECONNECT)", reconnect
        )
        self.assertNotIn("open_keyboard(peer->bda", reconnect)
        self.assertIn(
            "active_scan_mode == BLE_KEYBOARD_SCAN_RECONNECT", candidate
        )
        self.assertIn("bda_matches_remembered_peer", candidate)
        self.assertIn("BLE_KEYBOARD_RECONNECT_BACKOFF_MAX_MS", reconnect)

    def test_ble_reconnect_requires_connectable_advertisement_and_stops_scan(self):
        ble = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text(
            encoding="utf-8"
        )
        candidate_start = ble.index("static void consider_candidate(")
        candidate_end = ble.index("static bool key_in_report(", candidate_start)
        candidate = ble[candidate_start:candidate_end]
        callback_start = ble.index("static esp_err_t stop_scanning(")
        callback_end = ble.index("static const char *keyboard_layout_names", callback_start)
        callback = ble[callback_start:callback_end]
        scan_start = ble.index("static esp_err_t run_keyboard_scan(")
        scan_end = ble.index(
            "static esp_err_t close_connected_keyboard_for_pairing(",
            scan_start,
        )
        scan = ble[scan_start:scan_end]

        self.assertIn(
            "solar_os_ble_keyboard_scan_reconnect_event_is_connectable",
            candidate,
        )
        self.assertIn("reconnect_scan_stop_requested", candidate)
        self.assertIn("stop_scanning()", candidate)
        self.assertIn("ble_gap_disc_cancel()", callback)
        self.assertIn("xSemaphoreGive(scan_stop_done_sem)", callback)
        self.assertIn("xSemaphoreTake(scan_stop_done_sem", scan)
        self.assertLess(
            scan.index("xSemaphoreTake(scan_stop_done_sem"),
            scan.rindex("active_scan_mode = BLE_KEYBOARD_SCAN_DISCOVERY;"),
        )
        self.assertNotIn("open_keyboard(", callback)

    def test_ble_reconnect_preserves_matched_candidate_until_open(self):
        ble = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text(
            encoding="utf-8"
        )
        candidate_start = ble.index("static void consider_candidate(")
        candidate_end = ble.index("static bool key_in_report(", candidate_start)
        candidate = ble[candidate_start:candidate_end]
        open_start = ble.index("static esp_err_t scan_and_open_keyboard(")
        open_end = ble.index("static void scan_task(", open_start)
        open_path = ble[open_start:open_end]

        self.assertLess(
            candidate.index("if (candidate_frozen)"),
            candidate.index("bda_matches_remembered_peer"),
        )
        self.assertIn("candidate_frozen = true;", candidate)
        self.assertIn("ble_keyboard_candidate_t selected_candidate = {0};", open_path)
        self.assertIn("run_keyboard_scan(mode, &selected_candidate)", open_path)
        self.assertIn("open_keyboard(selected_candidate.bda", open_path)
        self.assertNotIn("open_keyboard(candidate.bda", open_path)

    def test_ble_pairing_retires_the_remembered_keyboard_bond(self):
        ble = (ROOT / "src/services/solar_os_ble_keyboard.c").read_text(
            encoding="utf-8"
        )
        pairing_start = ble.index("esp_err_t solar_os_ble_keyboard_start_pairing(")
        pairing_end = ble.index(
            "esp_err_t solar_os_ble_backend_prepare_sleep(", pairing_start
        )
        pairing = ble[pairing_start:pairing_end]
        finish_start = ble.index("static void finish_forget_operation(")
        finish_end = ble.index("static esp_err_t complete_bond_forget(", finish_start)
        finish = ble[finish_start:finish_end]

        self.assertIn("if (remembered_peer_count() > 0U)", pairing)
        self.assertIn("pairing_retry_pending = true;", pairing)
        self.assertIn("return forget_remembered_keyboard();", pairing)
        self.assertIn("pairing_retry_pending && result == ESP_OK", finish)
        self.assertIn("pairing_retry_pending = false;", finish)

    def test_ble_hid_replaces_a_conflicting_keyboard_bond(self):
        hid = (ROOT / "src/services/solar_os_ble_hid.c").read_text(
            encoding="utf-8"
        )
        replace_start = hid.index("static int replace_repeat_pairing_bond(")
        replace_end = hid.index("static int gap_callback(", replace_start)
        replace = hid[replace_start:replace_end]

        self.assertIn("ble_gap_conn_find", replace)
        self.assertIn("ble_store_util_delete_peer", replace)
        self.assertIn("return BLE_GAP_REPEAT_PAIRING_RETRY;", replace)
        self.assertIn(
            "case BLE_GAP_EVENT_REPEAT_PAIRING:\n"
            "        return replace_repeat_pairing_bond(event);",
            hid,
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

        self.assertIn("u8g2_ClearBuffer(session_overlay_u8g2);", overlay_request)
        self.assertNotIn(
            "solar_os_display_present(session_overlay_u8g2", overlay_request
        )

    def test_display_focus_shortcuts_are_global(self):
        main = (ROOT / "src/main.c").read_text(encoding="utf-8")
        sessions = (ROOT / "src/services/solar_os_sessions.c").read_text(
            encoding="utf-8"
        )
        dispatch = main.split("static void dispatch_input_key", 1)[1].split(
            "static void dispatch_input_pointer", 1
        )[0]

        ctrl_shortcut = dispatch.split(
            "if (ctrl_active && alt_active", 1
        )[1].split(
            "if (alt_active && !ctrl_active", 1
        )[0]
        self.assertIn("solar_os_sessions_cycle_display_focus()", ctrl_shortcut)
        self.assertIn(
            "solar_os_sessions_cycle_display_focus_previous()", ctrl_shortcut
        )
        self.assertIn(
            "solar_os_sessions_cycle_display_focus_down()", ctrl_shortcut
        )
        self.assertIn(
            "solar_os_sessions_cycle_display_focus_up()", ctrl_shortcut
        )
        self.assertNotIn("dispatch_key_to_input_focus", ctrl_shortcut)
        self.assertIn("SOLAR_OS_INPUT_MOD_CTRL", dispatch)
        self.assertIn("SOLAR_OS_INPUT_MOD_ALT", dispatch)
        self.assertIn("SOLAR_OS_KEY_CTRL_LEFT", dispatch)
        self.assertIn("SOLAR_OS_KEY_CTRL_RIGHT", dispatch)
        self.assertIn("SOLAR_OS_KEY_CTRL_UP", dispatch)
        self.assertIn("SOLAR_OS_KEY_CTRL_DOWN", dispatch)
        self.assertIn("if (alt_active && !ctrl_active", dispatch)
        self.assertIn("session_split_focus_neighbor(", sessions)
        self.assertIn("info.axis != axis", sessions)
        self.assertIn("session_store_input_focus(targets[next_index]);", sessions)

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

    def test_display_targets_use_u8g2_logical_geometry(self):
        display = (ROOT / "src/services/solar_os_display.c").read_text(
            encoding="utf-8"
        )
        board_display = (
            ROOT / "src/board/solar_os_board_display_expansion.c"
        ).read_text(encoding="utf-8")
        tft = (ROOT / "src/services/solar_os_tft_display.c").read_text(
            encoding="utf-8"
        )

        for source in (display, board_display):
            self.assertIn(
                "width != u8g2_GetDisplayWidth", source
            )
            self.assertIn(
                "height != u8g2_GetDisplayHeight", source
            )
        registered_geometry = tft.split(
            "device->display = (solar_os_board_display_t)", 1
        )[1].split(".surface_formats", 1)[0]
        self.assertIn(".width = u8g2_GetDisplayWidth(u8g2)", registered_geometry)
        self.assertIn(".height = u8g2_GetDisplayHeight(u8g2)", registered_geometry)
        self.assertNotIn("SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH", registered_geometry)

    def test_builtin_shell_follows_display_layouts(self):
        commands = (
            ROOT / "src/shell/solar_os_shell_commands.c"
        ).read_text(encoding="utf-8")
        sessions = (
            ROOT / "src/services/solar_os_sessions.c"
        ).read_text(encoding="utf-8")
        terminal = (
            ROOT / "src/services/solar_os_terminal.c"
        ).read_text(encoding="utf-8")

        split = commands.split("static void display_cmd_split", 1)[1].split(
            "static bool display_find_layout", 1
        )[0]
        destroy = commands.split(
            "static void display_cmd_destroy_layout", 1
        )[1].split("static void display_draw_test_pattern", 1)[0]
        self.assertIn("solar_os_sessions_builtin_shell_uses_display", split)
        self.assertIn("solar_os_sessions_rebind_builtin_shell_display", split)
        self.assertIn("solar_os_display_layout_unsplit(argv[2])", split)
        self.assertIn("solar_os_sessions_builtin_display_base", destroy)
        self.assertIn("solar_os_sessions_rebind_builtin_shell_display", destroy)
        self.assertIn("solar_os_terminal_rebind_display", sessions)
        self.assertIn("session_state.builtin_display_target", sessions)
        self.assertIn("terminal_apply_settings(terminal, false);", terminal)

    def test_runtime_displays_on_headless_boards_use_target_registry(self):
        display = (ROOT / "src/services/solar_os_display.c").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "bool solar_os_display_target_is_board_primary(const char *name)",
            display,
        )
        self.assertIn("return SOLAR_OS_BOARD_HAS_DISPLAY &&", display)

        drivers = (
            "solar_os_ssd1683.c",
            "solar_os_st7305_display.c",
            "solar_os_tft_display.c",
            "solar_os_cvbs_display.c",
            "solar_os_vga32_display.c",
        )
        for filename in drivers:
            source = (ROOT / "src/services" / filename).read_text(encoding="utf-8")
            self.assertIn(
                "device->primary = solar_os_display_target_is_board_primary(name);",
                source,
                filename,
            )
            self.assertNotIn(
                "device->primary = strcmp(name, SOLAR_OS_DISPLAY_PRIMARY_TARGET)",
                source,
                filename,
            )

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
