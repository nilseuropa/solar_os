from pathlib import Path
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


if __name__ == "__main__":
    unittest.main()
