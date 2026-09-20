from __future__ import annotations

from copy import deepcopy
from pathlib import Path
import sys
import tempfile
import tomllib
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from board_config import (
    available_base_profiles,
    profile_commands,
    profile_from_expansion,
    render_overlay,
)
from solaros_expansion_manifest import load_expansion_manifest
from solaros_board_manifest import (
    DriverBinding,
    DriverDef,
    ManifestError,
    generate_cmake,
    generate_header,
    load_board_manifest,
    load_driver_catalog,
    merge_board_overlay,
    required_packages,
    validate_board,
)


class BoardManifestTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.manifest_dir = ROOT / "boards" / "manifests"
        cls.drivers = load_driver_catalog(ROOT / "boards" / "expansion_drivers.toml")

    def test_all_manifests_validate(self) -> None:
        for path in self.manifest_dir.glob("*.toml"):
            with self.subTest(path=path.name):
                board = load_board_manifest(path, self.manifest_dir)
                validate_board(board, self.drivers)

    def test_display_manifests_define_logical_geometry(self) -> None:
        required = {
            "SOLAR_OS_BOARD_DISPLAY_CONTROLLER",
            "SOLAR_OS_BOARD_DISPLAY_WIDTH",
            "SOLAR_OS_BOARD_DISPLAY_HEIGHT",
        }
        for path in self.manifest_dir.glob("*.toml"):
            board = load_board_manifest(path, self.manifest_dir)
            if "display" not in board["build"]["capabilities"]:
                continue
            with self.subTest(path=path.name):
                self.assertLessEqual(required, set(board["defines"]))

    def test_native_display_geometry_must_match_logical_rotation(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "t_lora_pager.toml",
            self.manifest_dir,
        )
        invalid = deepcopy(board)
        invalid["defines"]["SOLAR_OS_BOARD_DISPLAY_HEIGHT"] = "320"
        with self.assertRaisesRegex(
            ManifestError,
            "logical display geometry does not match native geometry and rotation",
        ):
            validate_board(invalid, self.drivers)

        incomplete = deepcopy(board)
        del incomplete["defines"]["SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT"]
        with self.assertRaisesRegex(
            ManifestError,
            "display native width and height must be defined together",
        ):
            validate_board(incomplete, self.drivers)

    def test_both_mcu_families_have_a_base_profile(self) -> None:
        bases = available_base_profiles(self.manifest_dir)
        self.assertTrue(bases["esp32s3"])
        self.assertTrue(bases["esp32"])

    def test_custom_profile_commands_keep_the_base_environment_and_board(self) -> None:
        build, upload = profile_commands(
            "my_board",
            "esp32_s3_devkitc1_n16r8",
        )
        self.assertEqual(
            build,
            "SOLAR_OS_BOARD=my_board pio run -e esp32_s3_devkitc1_n16r8",
        )
        self.assertEqual(upload, f"{build} -t upload")

    def test_device_expansion_snapshot_promotes_to_valid_board_overlay(self) -> None:
        content = """\
schema = 1
kind = "solaros-expansion"

[base]
board = "waveshare_esp32_s3_sim7670g_4g"
firmware = "4.13.1"

[[buses]]
name = "spi0"
protocol = "spi"
sharing = "shared"
host = "SPI2_HOST"
sclk = 7
mosi = 8
miso = 3
cs = [9]
max_transfer_size = 4096

[[devices]]
driver = "ssd1683"
name = "display0"
bindings = { spi = "spi0", cs = 9, dc = 10, reset = 11, busy = 12 }
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "expansion.toml"
            path.write_text(content, encoding="utf-8")
            snapshot = load_expansion_manifest(path)
        base = load_board_manifest(
            self.manifest_dir / "waveshare_esp32_s3_sim7670g_4g.toml",
            self.manifest_dir,
        )
        profile = profile_from_expansion(
            snapshot,
            base,
            self.drivers,
            "waveshare_4g_epaper",
            "Waveshare 4G E-paper",
            "Custom",
            base["board"]["module"],
        )
        self.assertEqual(profile["extends"], base["board"]["id"])
        self.assertEqual(profile["buses"][0]["name"], "spi0")
        self.assertEqual(profile["devices"][0]["driver"], "ssd1683")
        self.assertIn("display_ssd1683", profile["build"]["drivers"])
        self.assertIn("display", profile["build"]["capabilities"])
        self.assertNotIn("SPI2_HOST", profile["runtime"]["spi_hosts"])
        self.assertEqual(
            {pin["gpio"] for pin in profile["pins"]},
            {3, 7, 8, 9, 10, 11, 12},
        )
        merged = merge_board_overlay(base, profile)
        validate_board(merged, self.drivers)
        header = generate_header(merged, self.drivers)
        self.assertIn("SOLAR_OS_BUS_ORIGIN_BOARD", header)
        self.assertIn('.driver = "ssd1683", .name = "display0"', header)

    def test_expansion_snapshot_rejects_manual_devices(self) -> None:
        content = """\
schema = 1
kind = "solaros-expansion"
[base]
board = "waveshare_esp32_s3_sim7670g_4g"
firmware = "4.13.1"
[[devices]]
driver = "manual"
name = "probe0"
bindings = { gpio = 7 }
"""
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "manual.toml"
            path.write_text(content, encoding="utf-8")
            with self.assertRaisesRegex(ManifestError, "cannot become board-owned"):
                load_expansion_manifest(path)

    def test_board_manifest_generates_midi_bus(self) -> None:
        base = load_board_manifest(
            self.manifest_dir / "waveshare_esp32_s3_sim7670g_4g.toml",
            self.manifest_dir,
        )
        overlay = {
            "buses": [{
                "name": "midi0",
                "protocol": "midi",
                "sharing": "exclusive",
                "port": "UART_NUM_2",
                "tx": 7,
                "rx": 8,
                "baud_rate": 31250,
            }],
            "pins": [
                {
                    "gpio": 7, "policy": "fixed", "role": "midi0 tx",
                    "user": False, "adc": False, "pwm": False,
                },
                {
                    "gpio": 8, "policy": "fixed", "role": "midi0 rx",
                    "user": False, "adc": False, "pwm": False,
                },
            ],
            "runtime": {"uart_ports": []},
        }
        board = merge_board_overlay(base, overlay)
        validate_board(board, self.drivers)
        header = generate_header(board, self.drivers)
        self.assertIn("SOLAR_OS_BUS_PROTOCOL_MIDI", header)
        self.assertIn(".baud_rate = 31250", header)

    def test_workbench_inherits_and_overrides_devkit(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "devkitc1_epaper_workbench.toml",
            self.manifest_dir,
        )
        self.assertEqual(board["target"]["mcu"], "esp32s3")
        self.assertEqual(
            {device["name"] for device in board["devices"]},
            {"keyboard0", "display0", "storage0"},
        )
        buses = {bus["name"]: bus for bus in board["buses"]}
        self.assertEqual(buses["spi0"]["cs"], [10, 6, 7])
        self.assertEqual(buses["spi1"]["host"], "SPI3_HOST")
        self.assertEqual(buses["spi1"]["sclk"], 1)
        self.assertEqual(buses["spi1"]["mosi"], 2)
        self.assertEqual(buses["spi1"]["miso"], 4)

    def test_generated_output_contains_fixed_expansions(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "devkitc1_epaper_workbench.toml",
            self.manifest_dir,
        )
        header = generate_header(board, self.drivers)
        cmake = generate_cmake(board, self.drivers)
        self.assertIn('.driver = "cardkb", .name = "keyboard0"', header)
        self.assertIn('.driver = "ssd1683", .name = "display0"', header)
        self.assertIn('.driver = "sdspi", .name = "storage0"', header)
        device_section = header.split(
            "#define SOLAR_OS_BOARD_DEFAULT_EXPANSION_DEVICES",
            1,
        )[1].split("#define SOLAR_OS_BOARD_CONNECTOR_LAYOUT_TITLE", 1)[0]
        self.assertNotIn("}}}},", device_section)
        self.assertIn("storage_expansion.cmake", cmake)
        self.assertIn("expansion_sdspi", required_packages(board, self.drivers))

    def test_elecrow_generated_header_preserves_buttons_and_tx_only_spi(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "elecrow_crowpanel_esp32_s3_4_2_epaper.toml",
            self.manifest_dir,
        )
        header = generate_header(board, self.drivers)
        self.assertIn('#include "solar_os_buttons.h"', header)
        self.assertIn("#define SOLAR_OS_BOARD_BUTTONS", header)
        self.assertIn(".miso_pin = GPIO_NUM_NC", header)

    def test_t_lora_exposes_a_real_spi_cs_and_claims_keyboard_pwm(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "t_lora_pager.toml",
            self.manifest_dir,
        )
        buses = {bus["name"]: bus for bus in board["buses"]}
        self.assertEqual(buses["spi0"]["cs"], [38, 21, 36, 39, 9])

        connectors = {pin["position"]: pin for pin in board["connectors"]}
        self.assertEqual(connectors[8]["gpio"], 9)
        self.assertNotIn("gpio", connectors[9])

        header = generate_header(board, self.drivers)
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_PWM, .role = "backlight", .value = 46',
            header,
        )
        self.assertIn(
            '.driver = "ublox-mia-m10q", .name = "gnss0"',
            header,
        )
        self.assertIn(
            '.driver = "xl9555", .name = "gpiox0"',
            header,
        )
        self.assertIn(
            '.driver = "drv2605", .name = "haptic0"',
            header,
        )
        self.assertIn(
            '.driver = "bq25896", .name = "charger0"',
            header,
        )
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, '
            '.role = "charge_current", .value = 704',
            header,
        )
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "output", '
            '.value = 7055',
            header,
        )
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "direction", '
            '.value = 58432',
            header,
        )
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_UART_PORT, '
            '.target = "gnss-uart", .value = UART_NUM_1',
            header,
        )
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "power", '
            '.target = "gpiox0", .value = 4',
            header,
        )
        self.assertIn(
            '.driver = "st25r3916", .name = "nfc0"',
            header,
        )
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "power", '
            '.target = "gpiox0", .value = 5',
            header,
        )
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "power", '
            '.target = "gpiox0", .value = 12',
            header,
        )
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "power", '
            '.target = "gpiox0", .value = 0',
            header,
        )
        packages = required_packages(board, self.drivers)
        self.assertIn("xl9555", packages)
        self.assertIn("tca8418", packages)
        self.assertIn("ublox_mia_m10q", packages)
        self.assertIn("st25r3916", packages)
        self.assertIn("drv2605", packages)
        self.assertIn("bq25896", packages)

    def test_t_deck_uses_canonical_uart_and_single_owner_system_key(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "t_deck_plus.toml",
            self.manifest_dir,
        )
        buses = {bus["name"]: bus for bus in board["buses"]}
        self.assertNotIn("uart1", buses)
        self.assertEqual(buses["uart0"]["port"], "UART_NUM_1")

        defines = board["defines"]
        self.assertEqual(
            defines["SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY"],
            "SOLAR_OS_KEY_ENTER",
        )
        self.assertNotIn("GPIO_NUM_0", defines["SOLAR_OS_BOARD_BUTTONS"])

        header = generate_header(board, self.drivers)
        self.assertIn(
            "#define SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY SOLAR_OS_KEY_ENTER",
            header,
        )
        self.assertIn(
            '.name = "uart0", .protocol = SOLAR_OS_BUS_PROTOCOL_UART',
            header,
        )
        self.assertIn(".config.uart = {.port = UART_NUM_1", header)
        self.assertIn(
            '.driver = "ublox-mia-m10q", .name = "gnss0"',
            header,
        )

        packages = required_packages(board, self.drivers)
        self.assertIn("ublox_mia_m10q", packages)

    def test_waveshare_sim7670_v2_claims_fixed_peripherals(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "waveshare_esp32_s3_sim7670g_4g.toml",
            self.manifest_dir,
        )
        buses = {bus["name"]: bus for bus in board["buses"]}
        self.assertEqual(
            (buses["modem-uart"]["port"],
             buses["modem-uart"]["tx"],
             buses["modem-uart"]["rx"],
             buses["modem-uart"]["baud_rate"]),
            ("UART_NUM_1", 18, 17, "115200"),
        )
        self.assertEqual(board["runtime"]["uart_ports"], ["UART_NUM_2"])
        self.assertEqual(
            {device["name"] for device in board["devices"]},
            {"storage0", "battery0", "pixels0", "modem0"},
        )

        pins = {pin["gpio"]: pin for pin in board["pins"]}
        self.assertEqual(
            {gpio for gpio, pin in pins.items() if pin["policy"] == "free"},
            {2, 3, 7, 8, 9, 10, 11, 12, 13, 14, 39, 40, 41, 42},
        )
        for gpio in (17, 18, 21, 38, 45, 46):
            self.assertEqual(pins[gpio]["policy"], "fixed")
        self.assertEqual(
            board["runtime"]["spi_hosts"],
            ["SPI2_HOST", "SPI3_HOST"],
        )
        self.assertEqual(board["runtime"]["i2s_ports"], ["I2S_NUM_1"])
        self.assertIn("expansion_spi", board["build"]["capabilities"])
        self.assertIn("expansion_i2s", board["build"]["capabilities"])

        header = generate_header(board, self.drivers)
        self.assertIn('.driver = "sim7670", .name = "modem0"', header)
        self.assertIn('.target = "modem-uart", .value = UART_NUM_1', header)
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "power", .value = 21',
            header,
        )
        self.assertIn('.driver = "max17048", .name = "battery0"', header)
        self.assertIn('.driver = "neopixel", .name = "pixels0"', header)
        self.assertIn('.driver = "sdmmc", .name = "storage0"', header)

        packages = required_packages(board, self.drivers)
        self.assertIn("sim7670", packages)
        self.assertIn("max17048", packages)
        self.assertIn("expansion_neopixel", packages)
        self.assertIn("expansion_sdmmc", packages)

    def test_solar_term_battery_binding_matches_runtime_driver(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "solar_term.toml",
            self.manifest_dir,
        )
        header = generate_header(board, self.drivers)
        runtime_driver = (
            ROOT / "src" / "services" / "solar_os_battery_adc_driver.c"
        ).read_text(encoding="utf-8")
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_ADC, .role = "adc", .value = 4',
            header,
        )
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_ADC, .role = "adc",',
            runtime_driver,
        )

    def test_cl32_manifest_keeps_m2_memory_and_system_signals_fixed(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "cl_32.toml",
            self.manifest_dir,
        )
        header = generate_header(board, self.drivers)
        self.assertNotIn("SOLAR_OS_BOARD_HEADLESS_PREFER_CDC", header)
        self.assertIn('#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "ST7305"', header)
        self.assertIn("#define SOLAR_OS_BOARD_DISPLAY_WIDTH 384", header)
        self.assertIn("#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 168", header)
        self.assertIn("display", board["build"]["capabilities"])
        self.assertIn("battery", board["build"]["capabilities"])
        self.assertIn("streaming_display", board["build"]["capabilities"])
        pins = {pin["gpio"]: pin for pin in board["pins"]}
        self.assertEqual(
            {gpio for gpio, pin in pins.items() if pin["policy"] == "free"},
            {4, 8, 15, 16, 17, 18, 21, 38, 39, 40, 41, 42, 47, 48},
        )
        for gpio in (1, 2, 3, 19, 20, 35, 36, 37, 46):
            self.assertEqual(pins[gpio]["policy"], "fixed")

        buses = {bus["name"]: bus for bus in board["buses"]}
        self.assertEqual(
            (buses["spi0"]["sclk"], buses["spi0"]["mosi"], buses["spi0"]["miso"]),
            (9, 10, 11),
        )
        self.assertEqual(buses["spi0"]["cs"], [6, 7])
        self.assertEqual(
            {device["name"] for device in board["devices"]},
            {"display0", "rtc0", "storage0", "audio0", "core0"},
        )
        display = next(
            device for device in board["devices"] if device["name"] == "display0"
        )
        self.assertEqual(display["driver"], "st7305")
        self.assertEqual(
            display["bindings"],
            {
                "spi": "spi0",
                "cs": 6,
                "dc": 13,
                "reset": 12,
                "panel": 1,
                "rotation": 3,
            },
        )
        audio = next(
            device for device in board["devices"] if device["name"] == "audio0"
        )
        self.assertEqual(audio["driver"], "audio-pwm")
        self.assertEqual(audio["bindings"], {"pwm": 5})
        self.assertIn("expansion_audio_pwm", required_packages(board, self.drivers))
        self.assertIn("driver_display_st7305", required_packages(board, self.drivers))
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_PWM, .role = "pwm", .value = 5',
            header,
        )
        self.assertIn(
            '.driver = "st7305", .name = "display0"',
            header,
        )
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "panel", .value = 1',
            header,
        )
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, .role = "rotation", .value = 3',
            header,
        )

        connectors = {
            (pin["connector"], pin["position"]): pin
            for pin in board["connectors"]
        }
        self.assertEqual(connectors[("EX1", 52)]["gpio"], 4)
        self.assertEqual(connectors[("EX1", 58)]["gpio"], 1)
        self.assertEqual(connectors[("EX1", 60)]["gpio"], 2)
        self.assertEqual(connectors[("EX1", 62)]["gpio"], 3)
        self.assertEqual(connectors[("EX1", 53)]["gpio"], 35)

    def test_solar_term_rtc_interrupt_binding_is_fixed_but_optional(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "solar_term.toml",
            self.manifest_dir,
        )
        rtc = next(device for device in board["devices"] if device["name"] == "rtc0")
        self.assertEqual(rtc["bindings"]["irq"], 15)
        rtc_pin = next(pin for pin in board["pins"] if pin["gpio"] == 15)
        self.assertEqual(rtc_pin["policy"], "fixed")
        self.assertEqual(rtc_pin["role"], "RTC interrupt")

        header = generate_header(board, self.drivers)
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_GPIO, .role = "irq", .value = 15',
            header,
        )
        rtc_adapter = (
            ROOT / "src" / "services" / "solar_os_pcf85063.c"
        ).read_text(encoding="utf-8")
        self.assertIn("solar_os_rtc_register_provider", rtc_adapter)
        self.assertNotIn("solar_os_time_register_provider", rtc_adapter)

        without_irq = deepcopy(board)
        rtc_without_irq = next(
            device for device in without_irq["devices"] if device["name"] == "rtc0"
        )
        del rtc_without_irq["bindings"]["irq"]
        validate_board(without_irq, self.drivers)

    def test_pin_conflict_is_rejected(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "devkitc1_epaper_workbench.toml",
            self.manifest_dir,
        )
        broken = deepcopy(board)
        display = next(device for device in broken["devices"] if device["name"] == "display0")
        display["bindings"]["dc"] = 12
        with self.assertRaisesRegex(ManifestError, "GPIO12 is shared"):
            validate_board(broken, self.drivers)

    def test_driver_packages_exist(self) -> None:
        with (ROOT / "packages" / "solar_os_packages.toml").open("rb") as file:
            packages = tomllib.load(file)["packages"]
        missing = sorted({driver.package for driver in self.drivers.values()} - set(packages))
        self.assertEqual(missing, [])

    def test_uart_device_binding_contains_declared_controller(self) -> None:
        drivers = dict(self.drivers)
        drivers["test-uart"] = DriverDef(
            name="test-uart",
            summary="test",
            package="test_uart",
            targets=("esp32s3",),
            capabilities=("expansion_uart",),
            board_capabilities=(),
            board_driver=None,
            board_defines={},
            early=False,
            default_name="uart-device0",
            bindings=(DriverBinding(
                key="uart",
                kind="uart_port",
                hint="bus",
                role=None,
                required=True,
                allowed=(),
                minimum=None,
                maximum=None,
            ),),
        )
        board = load_board_manifest(
            self.manifest_dir / "t_lora_pager.toml",
            self.manifest_dir,
        )
        board["devices"].append({
            "driver": "test-uart",
            "name": "uart-device0",
            "bindings": {"uart": "uart0"},
        })

        header = generate_header(board, drivers)

        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_UART_PORT, .target = "uart0", '
            '.value = UART_NUM_0',
            header,
        )

    def test_gpio_line_binding_contains_controller_and_line(self) -> None:
        drivers = dict(self.drivers)
        drivers["test-line"] = DriverDef(
            name="test-line",
            summary="test",
            package="test_line",
            targets=("esp32s3",),
            capabilities=(),
            board_capabilities=(),
            board_driver=None,
            board_defines={},
            early=False,
            default_name="line-device0",
            bindings=(DriverBinding(
                key="power",
                kind="gpio_line",
                hint="controller:line",
                role="power",
                required=True,
                allowed=(),
                minimum=0,
                maximum=15,
            ),),
        )
        board = load_board_manifest(
            self.manifest_dir / "t_lora_pager.toml",
            self.manifest_dir,
        )
        board["devices"].append({
            "driver": "test-line",
            "name": "line-device0",
            "bindings": {"power": "gpiox0:4"},
        })

        header = generate_header(board, drivers)

        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "power", '
            '.target = "gpiox0", .value = 4',
            header,
        )

        board["devices"][-1]["bindings"]["power"] = 13
        header = generate_header(board, drivers)
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_GPIO_LINE, .role = "power", '
            '.value = 13',
            header,
        )

        board["devices"][-1]["bindings"]["power"] = "gpiox0:16"
        with self.assertRaisesRegex(ManifestError, "above 15"):
            validate_board(board, drivers)

    def test_gpio_controller_line_cannot_replace_spi_cs(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "t_lora_pager.toml",
            self.manifest_dir,
        )
        storage = next(
            device for device in board["devices"] if device["name"] == "storage0"
        )
        storage["bindings"]["cs"] = "gpiox0:12"
        with self.assertRaisesRegex(ManifestError, "must be an integer"):
            validate_board(board, self.drivers)

    def test_overlay_renderer_round_trip_shape(self) -> None:
        overlay = {
            "schema": 1,
            "extends": "esp32_s3_devkitc1_n16r8",
            "board": {
                "id": "test_board",
                "name": "Test Board",
                "vendor": "Test",
                "module": "ESP32-S3-WROOM-1-N16R8",
            },
            "build": {"drivers": [], "capabilities": []},
            "devices": [{
                "driver": "cardkb",
                "name": "keyboard0",
                "bindings": {"i2c": "i2c0", "addr": 0x5F},
            }],
        }
        rendered = render_overlay(overlay)
        parsed = tomllib.loads(rendered)
        base = load_board_manifest(
            self.manifest_dir / "esp32_s3_devkitc1_n16r8.toml",
            self.manifest_dir,
        )
        merged = merge_board_overlay(base, parsed)
        validate_board(merged, self.drivers)
        self.assertEqual(parsed["devices"][0]["bindings"]["addr"], 0x5F)


if __name__ == "__main__":
    unittest.main()
