import contextlib
import io
import json
import os
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import compile_keyboard_layouts as compiler
import generate_keyboard_layouts as generator


def write_keyboard_layouts(directory) -> None:
    """Write the generated layout header that solar_os_input.c includes."""
    output = Path(directory, "solar_os_keyboard_layouts_generated.h")
    if compiler.main(["--output", str(output)]) != 0:
        raise RuntimeError("keyboard layout generation failed")


class CompileKeyboardLayoutsTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def write(self, stem, keys, **fields):
        data = {"name": stem, "description": stem,
                "source": {**generator.REQUIRED_VERSIONS, "layout": stem, "variant": ""},
                "keys": keys}
        data.update(fields)
        (self.directory / f"{stem}.json").write_text(json.dumps(data), encoding="utf-8")

    def test_builtin_layouts_compile_in_nvs_order(self):
        output = self.directory / "layouts.h"
        self.assertEqual(compiler.main(["--output", str(output)]), 0)
        text = output.read_text()
        self.assertIn("#define SOLAR_OS_KEYBOARD_LAYOUT_INDEX_US 0U", text)
        self.assertIn("#define SOLAR_OS_KEYBOARD_LAYOUT_INDEX_DE 1U", text)
        self.assertIn('{"us", "English (US)", false,', text)
        self.assertIn('{"de", "German", true,', text)

    def test_levels_caps_and_dead_keys(self):
        self.write("xx", {
            "0x04": {"levels": ["a", "A", "æ"], "caps": ["A", "a"]},
            "0x35": {"levels": [{"dead": "circumflex"}, "°", {"dead": "belowmacron"}]},
        })
        layout = compiler.load(self.directory, "xx")
        self.assertTrue(layout["altgr"])
        (usage_a, dead_a, chars_a), (usage_dead, dead, chars) = layout["keys"]
        self.assertEqual((usage_a, dead_a), (0x04, 0))
        self.assertEqual(chars_a, [0x61, 0x41, 0xE6, 0, 0x41, 0x61, 0, 0])
        self.assertEqual(usage_dead, 0x35)
        # Dead keys type their spacing character; unknown ones type nothing.
        self.assertEqual(chars[:3], [ord("^"), 0xB0, 0])
        # Without "caps", Caps Lock leaves the levels unchanged.
        self.assertEqual(chars[4:7], chars[:3])
        self.assertEqual(dead, 0b01010101)

    def test_rejects_invalid_files(self):
        cases = {
            "name": ({}, {"name": "other"}),
            "usage": ({"0x4f": {"levels": ["a"]}}, {}),
            "not hex": ({"a": {"levels": ["a"]}}, {}),
            "upper hex": ({"0X04": {"levels": ["a"]}}, {}),
            "two chars": ({"0x04": {"levels": ["ab"]}}, {}),
            "outside BMP": ({"0x04": {"levels": ["\U0001F600"]}}, {}),
            "control": ({"0x04": {"levels": ["\n"]}}, {}),
            "five levels": ({"0x04": {"levels": ["a", "b", "c", "d", "e"]}}, {}),
            "unknown field": ({"0x04": {"levels": ["a"], "tap": "b"}}, {}),
            "old libxkbcommon": ({}, {"source": {**generator.REQUIRED_VERSIONS,
                                                 "libxkbcommon": "1.6.0"}}),
            "no versions": ({}, {"source": {"layout": "xx", "variant": ""}}),
        }
        for label, (keys, fields) in cases.items():
            with self.subTest(label):
                self.write("xx", keys, **fields)
                with self.assertRaises(compiler.LayoutError):
                    compiler.load(self.directory, "xx")

    def test_committed_layouts_cover_only_layout_keys(self):
        for path in sorted(generator.LAYOUT_DIR.glob("*.json")):
            with self.subTest(path.name):
                data = json.loads(path.read_text(encoding="utf-8"))
                self.assertEqual(data["name"], path.stem)
                self.assertEqual(data["source"]["xkeyboard-config"],
                                 generator.REQUIRED_VERSIONS["xkeyboard-config"])
                self.assertEqual(data["source"]["libxkbcommon"],
                                 generator.REQUIRED_VERSIONS["libxkbcommon"])
                compiler.load(generator.LAYOUT_DIR, path.stem)


class ToolchainVersionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        (self.directory / "lib").mkdir()
        (self.directory / "lib/libxkbcommon.so").write_text("")
        (self.directory / "xkb/symbols").mkdir(parents=True)

    def write_pc(self, xkbcommon, xkb_data):
        (self.directory / "xkbcommon.pc").write_text(
            f"prefix={self.directory}\nlibdir=${{prefix}}/lib\n"
            f"Name: xkbcommon\nVersion: {xkbcommon}\n")
        (self.directory / "xkeyboard-config.pc").write_text(
            f"xkb_base={self.directory}/xkb\nName: XKeyboardConfig\nVersion: {xkb_data}\n")

    def find(self):
        with mock.patch.dict(os.environ, {"PKG_CONFIG_PATH": str(self.directory)}), \
                mock.patch.object(generator, "PKG_CONFIG_DIRS", ()):
            return generator.find_toolchain()

    def test_pinned_versions_are_accepted(self):
        self.write_pc(generator.REQUIRED_VERSIONS["libxkbcommon"],
                      generator.REQUIRED_VERSIONS["xkeyboard-config"])
        library, root = self.find()
        self.assertEqual(library, self.directory / "lib/libxkbcommon.so")
        self.assertEqual(root, self.directory / "xkb")

    def test_other_versions_are_refused(self):
        for xkbcommon, xkb_data in (("1.6.0", generator.REQUIRED_VERSIONS["xkeyboard-config"]),
                                    (generator.REQUIRED_VERSIONS["libxkbcommon"], "2.41")):
            with self.subTest(xkbcommon=xkbcommon, xkb_data=xkb_data):
                self.write_pc(xkbcommon, xkb_data)
                with self.assertRaisesRegex(generator.ToolchainError, "found, but"):
                    self.find()


def pinned_toolchain():
    try:
        return generator.find_toolchain()
    except generator.ToolchainError as error:
        return str(error)


TOOLCHAIN = pinned_toolchain()


@unittest.skipIf(isinstance(TOOLCHAIN, str), f"pinned XKB toolchain unavailable: {TOOLCHAIN}")
class GenerateKeyboardLayoutsTest(unittest.TestCase):
    def test_committed_layouts_match_xkb(self):
        with tempfile.TemporaryDirectory() as temporary:
            committed = sorted(generator.LAYOUT_DIR.glob("*.json"))
            self.assertTrue(committed)
            for path in committed:
                (Path(temporary) / path.name).write_text(path.read_text(encoding="utf-8"))
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(generator.main(["--all", "--output-dir", temporary]), 0)
            for path in committed:
                with self.subTest(path.name):
                    self.assertEqual((Path(temporary) / path.name).read_text(encoding="utf-8"),
                                     path.read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
