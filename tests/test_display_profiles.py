from copy import deepcopy
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from board_config import Configurator, profile_from_expansion, render_overlay
from generate_expansion_profiles import generate_header as generate_runtime_profiles
from solaros_board_manifest import (
    ManifestError, board_defines_for_bindings, display_profile_for_bindings,
    generate_header, load_board_manifest, load_driver_catalog, merge_board_overlay,
    validate_board,
)


class DisplayProfilesTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.drivers = load_driver_catalog(ROOT / "boards/expansion_drivers.toml")
        cls.manifests = ROOT / "boards/manifests"

    def geometry(self, driver, bindings):
        defines = board_defines_for_bindings(self.drivers[driver], bindings)
        return tuple(int(defines[f"SOLAR_OS_BOARD_DISPLAY_{axis}"]) for axis in ("WIDTH", "HEIGHT"))

    def test_supported_sizes_and_runtime_defaults(self):
        for driver, bindings, expected in (
            ("st7305", {}, (400, 300)),
            ("st7305", {"panel": 1}, (384, 168)),
            ("st7305", {"panel": 1, "rotation": 1}, (384, 168)),
            ("ssd1683", {}, (400, 300)),
            ("ssd1683", {"panel": 0}, (400, 300)),
            ("ssd1683", {"panel": 4}, (792, 272)),
            ("ssd1683", {"panel": 4, "rotation": 3}, (272, 792)),
            ("ssd1685", {"panel": 1, "rotation": 3}, (384, 168)),
            ("ssd1677", {"rotation": 3}, (480, 800)),
            ("uc8179", {"panel": 1, "rotation": 1}, (480, 800)),
            ("uc8279", {"panel": 2}, (800, 480)),
        ):
            with self.subTest(driver=driver, bindings=bindings):
                self.assertEqual(self.geometry(driver, bindings), expected)
        self.assertEqual(
            board_defines_for_bindings(self.drivers["ili9341"], {}),
            self.drivers["ili9341"].board_defines)
        self.assertIsNone(display_profile_for_bindings(self.drivers["cardkb"], {}))
        with self.assertRaisesRegex(ManifestError, "no display profile"):
            self.geometry("st7305", {"panel": 2})
        with self.assertRaisesRegex(ManifestError, "unsupported display rotation"):
            self.geometry("st7305", {"rotation": 0})

    def snapshot(self, driver, panel=None, rotation=None, *, auxiliary=False):
        base = load_board_manifest(self.manifests / "waveshare_esp32_s3_sim7670g_4g.toml", self.manifests)
        bindings = {"spi": "spi0", "cs": 9, "dc": 10, "reset": 11}
        if driver != "st7305":
            bindings["busy"] = 12
        if panel is not None:
            bindings["panel"] = panel
        if rotation is not None:
            bindings["rotation"] = rotation
        if auxiliary:
            base["build"]["capabilities"].append("display")
            base.setdefault("defines", {}).update({
                "SOLAR_OS_BOARD_DISPLAY_CONTROLLER": '"Existing"',
                "SOLAR_OS_BOARD_DISPLAY_WIDTH": "640",
                "SOLAR_OS_BOARD_DISPLAY_HEIGHT": "480",
            })
        snapshot = {
            "base": {"board": base["board"]["id"]},
            "buses": [{"name": "spi0", "protocol": "spi", "sharing": "shared",
                       "host": "SPI2_HOST", "sclk": 7, "mosi": 8, "miso": 3,
                       "cs": [9], "max_transfer_size": 4096}],
            "devices": [{"driver": driver, "name": "epd0" if auxiliary else "display0",
                         "bindings": bindings}],
        }
        profile = profile_from_expansion(snapshot, base, self.drivers,
                                         "profile_test", "Test", "Custom", base["board"]["module"])
        return base, profile

    def test_import_uses_panel_and_rotation_in_generated_header(self):
        for driver, panel, rotation, width, height in (
            ("st7305", 1, 3, 384, 168),
            ("ssd1683", 4, 0, 792, 272),
            ("ssd1683", 4, 1, 272, 792),
            ("ssd1685", 1, 3, 384, 168),
            ("ssd1677", None, 3, 480, 800),
        ):
            with self.subTest(driver=driver, rotation=rotation):
                base, profile = self.snapshot(driver, panel, rotation)
                merged = merge_board_overlay(base, profile)
                validate_board(merged, self.drivers)
                header = generate_header(merged, self.drivers)
                self.assertIn(f"#define SOLAR_OS_BOARD_DISPLAY_WIDTH {width}\n", header)
                self.assertIn(f"#define SOLAR_OS_BOARD_DISPLAY_HEIGHT {height}\n", header)
                self.assertIn(f"#define SOLAR_OS_BOARD_DISPLAY_U8G2_ROTATION U8G2_R{rotation}\n", header)
                # The saved overlay contains ordinary runtime bindings, no desktop-only names.
                self.assertNotIn("display_profiles", render_overlay(profile))
                broken = deepcopy(merged)
                broken["defines"]["SOLAR_OS_BOARD_DISPLAY_WIDTH"] = "123"
                with self.assertRaises(ManifestError):
                    validate_board(broken, self.drivers)

    def test_board_rejects_geometry_that_disagrees_with_panel_selector(self):
        board = load_board_manifest(self.manifests / "cl_32.toml", self.manifests)
        display = next(device for device in board["devices"] if device["name"] == "display0")
        display["bindings"]["panel"] = 0
        # Legacy manifests need no explicit native-size macros. Their logical size
        # must still agree with the panel selected by ordinary runtime bindings.
        self.assertNotIn("SOLAR_OS_BOARD_DISPLAY_NATIVE_WIDTH", board["defines"])
        with self.assertRaisesRegex(ManifestError, "selected panel profile and rotation"):
            validate_board(board, self.drivers)

    def test_auxiliary_profile_preserves_existing_primary_geometry(self):
        base, profile = self.snapshot("ssd1685", 1, 3, auxiliary=True)
        self.assertNotIn("SOLAR_OS_BOARD_DISPLAY_WIDTH", profile.get("defines", {}))
        merged = merge_board_overlay(base, profile)
        validate_board(merged, self.drivers)
        self.assertEqual(merged["defines"]["SOLAR_OS_BOARD_DISPLAY_WIDTH"], "640")

    def test_tui_selects_named_profile_and_matching_rotation(self):
        class Screen:
            def __init__(self, profile, rotation):
                self.profile, self.rotation = profile, rotation
                self.choices = []

            def prompt(self, title, label, default=""):
                return default

            def choose(self, title, choices):
                self.choices.append((title, choices))
                choice = self.rotation if title.endswith(": rotation") else self.profile
                assert choice in dict(choices)
                return choice

            def yes_no(self, title, label, default=False):
                return False

        base_path = self.manifests / "esp32_s3_devkitc1_n16r8.toml"
        base = load_board_manifest(base_path, self.manifests)
        for name, selected, rotation, panel, expected in (
            ("st7305", "168x384", "3", 1, (384, 168)),
            ("ssd1683", "elecrow-579-dual", "1", 4, (272, 792)),
            ("ssd1306", "128x32-univision", "1", 1, (32, 128)),
            ("sh1106", "72x40-wise", "2", 3, (72, 40)),
        ):
            screen = Screen(selected, rotation)
            configurator = Configurator(screen, base_path, base, self.drivers)
            wiring = {"spi": "spi0", "cs": 10, "dc": 17, "reset": 16, "busy": 15,
                      "i2c": "i2c0", "addr": 0x3c}
            with patch.object(configurator, "_binding_value",
                              side_effect=lambda driver, spec, bindings: wiring[spec.key]):
                profile = configurator.create("test", "Test", "Custom", "Module", [name])
            bindings = profile["devices"][0]["bindings"]
            self.assertEqual(bindings["panel"], panel)
            self.assertEqual(bindings["rotation"], int(rotation))
            if "display" in self.drivers[name].board_capabilities:
                self.assertEqual(self.geometry(name, bindings), expected)
                self.assertEqual(int(profile["defines"]["SOLAR_OS_BOARD_DISPLAY_WIDTH"]), expected[0])
            else:
                self.assertNotIn("SOLAR_OS_BOARD_DISPLAY_WIDTH", profile.get("defines", {}))
            self.assertIn(f"{expected[0]}x{expected[1]}", str(screen.choices[-1]))

    def test_oled_profiles_preserve_primary_geometry_and_validate_bindings(self):
        for driver, count in (("ssd1306", 8), ("sh1106", 4)):
            with self.subTest(driver=driver):
                definition = self.drivers[driver]
                self.assertEqual(len(definition.display_profiles), count)
                self.assertEqual(board_defines_for_bindings(definition, {"panel": 1, "rotation": 3}), {})
                board = load_board_manifest(self.manifests / "cl_32.toml", self.manifests)
                original = dict(board["defines"])
                board["devices"].append({"driver": driver, "name": "oled0",
                    "bindings": {"i2c": "i2c0", "addr": 0x3c, "panel": 1, "rotation": 3}})
                validate_board(board, self.drivers)
                self.assertEqual(board["defines"], original)
                board["devices"][-1]["bindings"]["panel"] = count
                with self.assertRaises(ManifestError):
                    validate_board(board, self.drivers)

    def test_runtime_panel_labels_follow_the_catalog_and_enabled_packages(self):
        catalog = ROOT / "boards/expansion_drivers.toml"
        header = generate_runtime_profiles(catalog, {"expansion_ssd1306"})
        self.assertEqual(header.count("    X("), 12)
        self.assertIn('X("ssd1306", 1, "128x32 Univision", 0)', header)
        self.assertIn('X("sh1106", 3, "72x40 Wise", 0)', header)
        self.assertNotIn('X("st7305"', header)
        self.assertNotIn('X("ssd1683"', header)
        epd = generate_runtime_profiles(catalog, {"expansion_ssd1683"})
        self.assertIn('X("ssd1683", 3, "400x300 Waveshare 4.2-inch V2", 1)', epd)
        self.assertIn('X("ssd1683", 4, "792x272 Elecrow 5.79-inch dual controller", 0)', epd)
        self.assertNotIn("    X(", generate_runtime_profiles(catalog, set()))

    def test_invalid_catalog_profiles_are_rejected(self):
        catalog = '''schema = 1
[drivers.test]
package = "display_test"
targets = ["esp32s3"]
[[drivers.test.bindings]]
key = "panel"
kind = "parameter"
allowed = [0, 1]
[[drivers.test.bindings]]
key = "rotation"
kind = "parameter"
min = 0
max = 3
[[drivers.test.display_profiles]]
name = "small"
width = 168
height = 384
rotation = 3
bindings = { panel = 0 }
default = true
[[drivers.test.display_profiles]]
name = "large"
width = 300
height = 400
rotation = 1
bindings = { panel = 1 }
'''
        for old, new in (
            ("width = 168", "width = 0"),
            ("height = 384", "height = true"),
            ("rotation = 3", "rotation = 4"),
            ('name = "large"', 'name = "small"'),
            ("panel = 1", "panel = 0"),
            ("panel = 1", "panel = 2"),
            ("panel = 1", "absent = 1"),
            ("panel = 1", "rotation = 1"),
            ("bindings = { panel = 1 }", "bindings = {}"),
            ("default = true", "default = false"),
            ("default = true", 'default = "yes"'),
            ('name = "large"', 'name = "large"\ndefault = true'),
        ):
            with self.subTest(change=new), tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / "catalog.toml"
                path.write_text(catalog.replace(old, new))
                with self.assertRaises(ManifestError):
                    load_driver_catalog(path)


if __name__ == "__main__":
    unittest.main()
