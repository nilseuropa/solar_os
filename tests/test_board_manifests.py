from __future__ import annotations

from copy import deepcopy
import json
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

    def test_inputronic_keyboard_overlay_uses_named_bus(self) -> None:
        for profile in ("esp32_s3_devkitc1_n16r8", "esp32_devkitc_v4_wrover", "cl_32"):
            with self.subTest(profile=profile):
                board = load_board_manifest(
                    self.manifest_dir / f"{profile}.toml", self.manifest_dir)
                board.setdefault("devices", []).append({
                    "driver": "inputronic-keyboard", "name": "keyboard1",
                    "bindings": {"i2c": "i2c0", "addr": 0x34},
                })
                validate_board(board, self.drivers)
                self.assertIn("expansion_inputronic_keyboard", required_packages(board, self.drivers))
                self.assertIn('.driver = "inputronic-keyboard", .name = "keyboard1"',
                              generate_header(board, self.drivers))
                board["devices"][-1]["bindings"]["addr"] = 0x35
                with self.assertRaises(ManifestError):
                    validate_board(board, self.drivers)

    def test_tab5_keyboard_named_bus_optional_irq_and_address_range(self) -> None:
        for profile in ("esp32_s3_devkitc1_n16r8", "esp32_devkitc_v4_wrover"):
            board = load_board_manifest(self.manifest_dir / f"{profile}.toml", self.manifest_dir)
            device = {"driver": "tab5-keyboard", "name": "keyboard1",
                      "bindings": {"i2c": "i2c0", "addr": 0x6d}}
            board.setdefault("devices", []).append(device)
            validate_board(board, self.drivers)
            self.assertIn("expansion_tab5_keyboard", required_packages(board, self.drivers))
            self.assertIn('.driver = "tab5-keyboard", .name = "keyboard1"',
                          generate_header(board, self.drivers))
            if profile == "esp32_s3_devkitc1_n16r8":
                device["bindings"]["irq"] = 1
                pin = next(pin for pin in board["pins"] if pin["gpio"] == 1)
                pin.update(policy="fixed", adc=False, pwm=False)
                validate_board(board, self.drivers)
            for address in (0x08, 0x77):
                device["bindings"]["addr"] = address
                validate_board(board, self.drivers)
            for address in (0x07, 0x78):
                device["bindings"]["addr"] = address
                with self.assertRaises(ManifestError):
                    validate_board(board, self.drivers)

    def test_generic_tca8418_geometry_and_pager_profile(self) -> None:
        board = load_board_manifest(self.manifest_dir / "esp32_devkitc_v4_wrover.toml",
                                    self.manifest_dir)
        board.setdefault("devices", []).append({
            "driver": "tca8418", "name": "keyboard1",
            "bindings": {"i2c": "i2c0", "addr": 0x34, "rows": 3, "cols": 3},
        })
        validate_board(board, self.drivers)
        self.assertIn("tca8418", required_packages(board, self.drivers))
        for key, value in (("rows", 9), ("cols", 0)):
            with self.subTest(key=key):
                previous = board["devices"][-1]["bindings"][key]
                board["devices"][-1]["bindings"][key] = value
                with self.assertRaises(ManifestError):
                    validate_board(board, self.drivers)
                board["devices"][-1]["bindings"][key] = previous
        pager = load_board_manifest(self.manifest_dir / "t_lora_pager.toml", self.manifest_dir)
        keyboard = next(device for device in pager["devices"] if device["name"] == "keyboard0")
        self.assertEqual(keyboard["driver"], "lilygo-pager-keyboard")
        self.assertEqual(keyboard["bindings"]["backlight"], 46)
        self.assertIn('.driver = "lilygo-pager-keyboard"', generate_header(pager, self.drivers))

    def test_4g_epaper_initializes_tab5_on_board_bus(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "waveshare_esp32_s3_sim7670g_4g_epaper.toml",
            self.manifest_dir)
        keyboard = next(device for device in board["devices"]
                        if device["name"] == "keyboard0")
        self.assertEqual(keyboard["driver"], "tab5-keyboard")
        self.assertEqual(keyboard["bindings"],
                         {"i2c": "i2c0", "addr": 0x6d, "irq": 41})
        self.assertIn("expansion_tab5_keyboard", required_packages(board, self.drivers))
        self.assertEqual(next(pin for pin in board["pins"] if pin["gpio"] == 40)["policy"], "free")
        self.assertNotIn("expansion_cardkb", required_packages(board, self.drivers))
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

    def test_goouuu_esp32_s3cam_pin_contract(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "goouuu_esp32_s3cam.toml",
            self.manifest_dir,
        )
        self.assertIn("camera", board["build"]["capabilities"])
        self.assertIn(
            "driver_camera_esp32",
            required_packages(board, self.drivers),
        )
        self.assertEqual(board["build"]["psram_bytes"], 8 * 1024 * 1024)
        pins = {pin["gpio"]: pin for pin in board["pins"]}
        self.assertEqual(
            {gpio for gpio, pin in pins.items() if pin["policy"] == "free"},
            {1, 14, 21, 41, 42, 47},
        )
        devices = {device["name"]: device for device in board["devices"]}
        self.assertEqual(
            devices["storage0"]["bindings"],
            {"clk": 39, "cmd": 38, "d0": 40},
        )
        self.assertEqual(devices["pixels0"]["bindings"], {"data": 48, "count": 1})
        self.assertEqual(devices["camera0"]["driver"], "esp32-camera")
        self.assertEqual(devices["camera0"]["bindings"], {
            "d0": 11, "d1": 9, "d2": 8, "d3": 10, "d4": 12,
            "d5": 18, "d6": 17, "d7": 16, "siod": 4, "sioc": 5,
            "vsync": 6, "href": 7, "pclk": 13, "xclk": 15,
        })
        self.assertIn('.driver = "esp32-camera", .name = "camera0"',
                      generate_header(board, self.drivers))

    def test_camera_accepts_named_board_bus_and_rejects_mixed_control_bindings(self) -> None:
        board = load_board_manifest(self.manifest_dir / "goouuu_esp32_s3cam.toml",
                                    self.manifest_dir)
        board["buses"].append({"name": "i2c0", "protocol": "i2c", "sharing": "shared",
                                "port": "I2C_NUM_0", "sda": 4, "scl": 5,
                                "speed_hz": 100000})
        camera = next(device for device in board["devices"] if device["name"] == "camera0")
        camera["bindings"].pop("siod")
        camera["bindings"].pop("sioc")
        camera["bindings"]["i2c"] = "i2c0"
        validate_board(board, self.drivers)
        self.assertIn('.target = "i2c0"', generate_header(board, self.drivers))
        for control in ({}, {"siod": 4}, {"sioc": 5},
                        {"i2c": "i2c0", "siod": 4},
                        {"i2c": "i2c0", "sioc": 5},
                        {"i2c": "i2c0", "siod": 4, "sioc": 5}):
            with self.subTest(control=control):
                broken = deepcopy(board)
                bindings = next(device["bindings"] for device in broken["devices"]
                                if device["name"] == "camera0")
                bindings.pop("i2c")
                bindings.update(control)
                with self.assertRaisesRegex(ManifestError, "either i2c or both siod and sioc"):
                    validate_board(broken, self.drivers)

    def test_qdtech_es3n28p_uses_non_touch_fixed_hardware(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "qdtech_es3n28p.toml",
            self.manifest_dir,
        )
        self.assertNotIn("pointer", board["build"]["capabilities"])
        self.assertNotIn("pointer_ft6336", board["build"]["drivers"])
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_DISPLAY_INVERT_COLOR"], "1")
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH"], "240")
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT"], "320")
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_DISPLAY_MADCTL"], "0x48")
        self.assertEqual(
            board["defines"]["SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION"],
            "U8G2_R1",
        )
        buses = {bus["name"]: bus for bus in board["buses"]}
        self.assertEqual(buses["spi0"]["miso"], 13)
        devices = {device["name"]: device for device in board["devices"]}
        self.assertEqual(
            set(devices),
            {"display0", "battery0", "audio0", "storage0", "pixels0"},
        )
        self.assertEqual(devices["display0"]["driver"], "ili9341")
        self.assertEqual(devices["pixels0"]["bindings"], {"data": 42, "count": 1})
        hardware = json.loads(
            (ROOT / "boards/qdtech_es3n28p.json").read_text(encoding="utf-8")
        )
        self.assertEqual(board["target"]["platformio_board"], "qdtech_es3n28p")
        self.assertEqual(hardware["build"]["flash_mode"], "dio")
        self.assertEqual(hardware["build"]["psram_type"], "opi")

    def test_qdtech_es3c28p_adds_ft6336_touch(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "qdtech_es3c28p.toml",
            self.manifest_dir,
        )
        self.assertIn("pointer", board["build"]["capabilities"])
        self.assertIn("pointer_ft6336", board["build"]["drivers"])
        self.assertEqual(board["target"]["platformio_board"], "qdtech_es3n28p")
        self.assertEqual(
            board["defines"]["SOLAR_OS_BOARD_PIN_TOUCH_INT"],
            "GPIO_NUM_17",
        )
        self.assertEqual(
            board["defines"]["SOLAR_OS_BOARD_PIN_TOUCH_RST"],
            "GPIO_NUM_18",
        )
        devices = {device["name"]: device for device in board["devices"]}
        self.assertEqual(
            set(devices),
            {"display0", "touch0", "battery0", "audio0", "storage0", "pixels0"},
        )
        self.assertEqual(devices["touch0"]["driver"], "ft6336")
        self.assertEqual(
            devices["touch0"]["bindings"],
            {
                "i2c": "i2c0",
                "addr": 0x38,
                "reset": 18,
                "irq": 17,
                "rotation": 1,
            },
        )
        pins = {pin["gpio"]: pin for pin in board["pins"]}
        self.assertEqual(pins[17]["role"], "touch interrupt")
        self.assertEqual(pins[18]["role"], "touch reset")

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
driver = "cardkb"
name = "keyboard0"
bindings = { i2c = "i2c0", addr = 0x5f }

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
        self.assertEqual(
            [(device["driver"], device["name"]) for device in profile["devices"]],
            [("cardkb", "keyboard0"), ("ssd1683", "display0")],
        )
        self.assertIn("display_ssd1683", profile["build"]["drivers"])
        self.assertIn("display", profile["build"]["capabilities"])
        self.assertNotIn("SPI2_HOST", profile["runtime"]["spi_hosts"])
        self.assertEqual(
            {pin["gpio"] for pin in profile["pins"]},
            {3, 7, 8, 9, 10, 11, 12},
        )
        merged = merge_board_overlay(base, profile)
        validate_board(merged, self.drivers)
        self.assertIn("expansion_cardkb", required_packages(merged, self.drivers))
        header = generate_header(merged, self.drivers)
        self.assertIn("SOLAR_OS_BUS_ORIGIN_BOARD", header)
        self.assertIn('.driver = "cardkb", .name = "keyboard0"', header)
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

    def test_elecrow_579_uses_dual_controller_geometry(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "elecrow_crowpanel_esp32_s3_5_79_epaper.toml",
            self.manifest_dir,
        )
        header = generate_header(board, self.drivers)
        self.assertIn('#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "SSD1683x2"', header)
        self.assertIn("#define SOLAR_OS_BOARD_DISPLAY_WIDTH 792", header)
        self.assertIn("#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 272", header)
        self.assertIn('.role = "panel", .value = 4', header)
        self.assertIn(".miso_pin = GPIO_NUM_NC", header)

        too_many_bindings = deepcopy(board)
        for i in range(17):
            too_many_bindings["devices"][0]["bindings"][f"extra{i}"] = 0
        with self.assertRaisesRegex(ManifestError, "more than 16 bindings"):
            validate_board(too_many_bindings, self.drivers)

    def test_x4_pro_claims_fixed_peripherals_and_neutral_frontlight(self) -> None:
        board = load_board_manifest(self.manifest_dir / "xteink_x4_pro.toml", self.manifest_dir)
        validate_board(board, self.drivers)
        devices = {device["name"]: device for device in board["devices"]}
        self.assertEqual(devices["display0"]["driver"], "x4pro")
        self.assertEqual(devices["display0"]["bindings"], {
            "spi": "spi0", "cs": 13, "dc": 18, "reset": 14, "busy": 6,
            "latch": 1, "cool": 8, "warm": 9,
        })
        self.assertEqual(devices["storage0"]["bindings"], {
            "clk": 41, "cmd": 42, "d0": 40, "power": 5, "active": 0,
        })
        self.assertEqual(devices["touch0"]["bindings"], {
            "i2c": "i2c0", "addr": 0x5d, "alt_addr": 0x14, "irq": 10,
            "reset": 4, "power": 2, "active": 0, "rotation": 1, "home_key": 0x92,
        })
        self.assertEqual(devices["rtc0"]["bindings"], {"i2c": "i2c0", "addr": 0x51})
        self.assertEqual(devices["battery0"]["bindings"], {"i2c": "i2c0", "addr": 0x63})
        buses = {bus["name"]: bus for bus in board["buses"]}
        self.assertEqual((buses["spi0"]["sclk"], buses["spi0"]["mosi"]), (12, 11))
        self.assertNotIn("miso", buses["spi0"])
        self.assertEqual((buses["i2c0"]["sda"], buses["i2c0"]["scl"]), (39, 38))
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_DISPLAY_DEFAULT_ORIENTATION"], "0")
        self.assertIn("display_brightness", board["build"]["capabilities"])
        self.assertTrue(all(pin["policy"] == "fixed" for pin in board["pins"]))
        self.assertFalse(board.get("connectors"))
        self.assertLessEqual({"expansion_x4pro", "cw2017", "driver_pcf8563",
                              "driver_gt911", "expansion_sdmmc"},
                             set(required_packages(board, self.drivers)))
        header = generate_header(board, self.drivers)
        self.assertLess(header.index('.driver = "x4pro"'), header.index('.driver = "sdmmc"'))
        self.assertIn("set(SOLAR_OS_BOARD_HAS_DISPLAY_BRIGHTNESS ON)",
                      generate_cmake(board, self.drivers))
        memory = json.loads((ROOT / "boards/xteink_x4_pro.json").read_text())
        self.assertEqual(memory["build"]["flash_mode"], "qio")
        self.assertEqual(memory["build"]["psram_type"], "opi")
        self.assertEqual(memory["upload"]["flash_size"], "16MB")

    def test_waveshare_397_uses_ssd1677_portrait_profile(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "waveshare_esp32_s3_epaper_3_97.toml",
            self.manifest_dir,
        )
        self.assertIn("battery", board["build"]["capabilities"])
        self.assertNotIn("imu", board["build"]["capabilities"])
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH"], "800")
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_DISPLAY_NATIVE_HEIGHT"], "480")
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_DISPLAY_WIDTH"], "480")
        self.assertEqual(board["defines"]["SOLAR_OS_BOARD_DISPLAY_HEIGHT"], "800")
        self.assertEqual(
            board["defines"]["SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION"],
            "U8G2_R3",
        )
        self.assertEqual(
            board["defines"]["SOLAR_OS_BOARD_DISPLAY_DEFAULT_ORIENTATION"],
            "270",
        )

        devices = {device["name"]: device for device in board["devices"]}
        self.assertEqual(
            set(devices),
            {
                "power0", "imu0", "display0", "rtc0", "environment0",
                "audio0", "storage0",
            },
        )
        self.assertEqual(devices["power0"]["driver"], "axp2101")
        self.assertEqual(
            devices["power0"]["bindings"],
            {
                "i2c": "i2c0", "addr": 0x34, "input_current": 1500,
                "charge_current": 200, "charge_voltage": 4200,
            },
        )
        self.assertEqual(devices["imu0"]["driver"], "qmi8658")
        self.assertEqual(
            devices["imu0"]["bindings"],
            {"i2c": "i2c0", "addr": 0x6A},
        )
        self.assertEqual(devices["display0"]["driver"], "ssd1677")
        self.assertEqual(
            devices["display0"]["bindings"],
            {
                "spi": "spi0", "cs": 10, "dc": 9, "reset": 46,
                "busy": 3, "rotation": 3, "power_i2c": "i2c0",
                "power_addr": 0x34,
            },
        )
        self.assertEqual(
            devices["storage0"]["bindings"],
            {"clk": 16, "cmd": 17, "d0": 15, "d1": 7, "d2": 8, "d3": 18},
        )
        self.assertEqual(
            devices["audio0"]["bindings"],
            {
                "i2c": "i2c0", "i2s": 0, "mclk": 13, "bck": 14,
                "ws": 47, "din": 48, "dout": 21, "pa": 39,
            },
        )

        header = generate_header(board, self.drivers)
        self.assertIn('#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "SSD1677"', header)
        self.assertIn('#define SOLAR_OS_BOARD_DISPLAY_DEFAULT_ORIENTATION 270', header)
        self.assertIn('.driver = "ssd1677", .name = "display0"', header)
        self.assertIn('.role = "rotation", .value = 3', header)
        self.assertIn(
            'SOLAR_OS_EXPANSION_BINDING_I2C_BUS, .role = "power", .target = "i2c0"',
            header,
        )
        self.assertIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_PARAMETER, '
            '.role = "power_addr", .value = 52', header,
        )
        self.assertNotIn(
            '.kind = SOLAR_OS_EXPANSION_BINDING_I2C_ADDRESS, '
            '.role = "power"', header,
        )
        self.assertFalse(self.drivers["qmi8658"].early)
        self.assertIn("expansion_ssd1677", required_packages(board, self.drivers))

        hardware = json.loads(
            (ROOT / "boards/waveshare_esp32_s3_epaper_3_97.json").read_text(
                encoding="utf-8"
            )
        )
        self.assertEqual(hardware["upload"]["flash_size"], "16MB")
        self.assertEqual(hardware["build"]["psram_type"], "opi")

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
        self.assertIn("expansion_lilygo_pager_keyboard", packages)
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
        runtime_driver = (
            ROOT / "src" / "services" / "solar_os_cl32_core.c"
        ).read_text(encoding="utf-8")
        self.assertNotIn("SOLAR_OS_BOARD_HEADLESS_PREFER_CDC", header)
        self.assertIn('#define SOLAR_OS_BOARD_DISPLAY_CONTROLLER "ST7305"', header)
        self.assertIn("#define SOLAR_OS_BOARD_DISPLAY_WIDTH 384", header)
        self.assertIn("#define SOLAR_OS_BOARD_DISPLAY_HEIGHT 168", header)
        self.assertIn(
            "#define CL32_CORE_REG_BATTERY_VOLTAGE 0x14U",
            runtime_driver,
        )
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
