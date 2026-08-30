from __future__ import annotations

from copy import deepcopy
from pathlib import Path
import sys
import tomllib
import unittest


ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from board_config import available_base_profiles, profile_commands, render_overlay
from solaros_board_manifest import (
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

    def test_waveshare_battery_binding_matches_runtime_driver(self) -> None:
        board = load_board_manifest(
            self.manifest_dir / "waveshare_esp32_s3_rlcd_4_2.toml",
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
