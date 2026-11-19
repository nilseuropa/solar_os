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
        self.assertIn("#define SOLAR_OS_KEYBOARD_LAYOUT_INDEX_UA 2U", text)
        self.assertIn("#define SOLAR_OS_KEYBOARD_LAYOUT_INDEX_FR 3U", text)
        self.assertIn("#define SOLAR_OS_KEYBOARD_LAYOUT_INDEX_RO 4U", text)
        self.assertIn('{"us", "English (US)", false,', text)
        self.assertIn('{"de", "German", true,', text)
        self.assertIn('{"ua", "Ukrainian", true,', text)

    def test_levels_caps_and_dead_keys(self):
        self.write("xx", {
            "0x04": {"levels": ["a", "A", "æ"], "caps": ["A", "a"]},
            "0x35": {"levels": [{"dead": "circumflex"}, "°", {"dead": "belowmacron"}]},
        }, dead={"circumflex": {" ": "^", "a": "â", "A": "Â"}, "belowmacron": {}})
        layout = compiler.load(self.directory, "xx")
        self.assertTrue(layout["altgr"])
        (usage_a, chars_a), (usage_dead, chars) = layout["keys"]
        self.assertEqual(usage_a, 0x04)
        self.assertEqual(chars_a, [0x61, 0x41, 0xE6, 0, 0x41, 0x61, 0, 0])
        self.assertEqual(usage_dead, 0x35)
        self.assertEqual(chars[:3], [("dead", "circumflex"), 0xB0, ("dead", "belowmacron")])
        # Without "caps", Caps Lock leaves the levels unchanged.
        self.assertEqual(chars[4:7], chars[:3])
        self.assertEqual(layout["dead"]["circumflex"], {0x20: 0x5E, 0x61: 0xE2, 0x41: 0xC2})

        text = compiler.header([layout])
        # Dead keys are indexes into one table, sorted by name; the space entry
        # is the spacing character, the rest are compositions sorted by base.
        self.assertIn("    {0x35, 0x55, {1, 0x00b0, 0, 0x0000, 1, 0x00b0, 0, 0x0000}},", text)
        self.assertIn("    {0x0000, 0, 0}, /* dead_belowmacron */", text)
        self.assertIn("    {0x005e, 0, 2}, /* dead_circumflex */", text)
        self.assertLess(text.index("{0x0041, 0x00c2}"), text.index("{0x0061, 0x00e2}"))

    def test_dead_keys_merge_across_layouts(self):
        self.write("aa", {"0x35": {"levels": [{"dead": "grave"}]}}, dead={"grave": {"a": "à"}})
        self.write("bb", {"0x35": {"levels": [{"dead": "grave"}]}}, dead={"grave": {"e": "è"}})
        layouts = [compiler.load(self.directory, name) for name in ("aa", "bb")]
        self.assertEqual(compiler.merge_dead_keys(layouts), {"grave": {0x61: 0xE0, 0x65: 0xE8}})
        self.write("bb", {"0x35": {"levels": [{"dead": "grave"}]}}, dead={"grave": {"a": "á"}})
        layouts[1] = compiler.load(self.directory, "bb")
        with self.assertRaises(compiler.LayoutError):
            compiler.merge_dead_keys(layouts)

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
            "old libx11": ({}, {"source": {**generator.REQUIRED_VERSIONS, "libx11": "1.8.7"}}),
            "dead key without table": ({"0x35": {"levels": [{"dead": "grave"}]}}, {}),
            "unused dead table": ({}, {"dead": {"grave": {}}}),
            "dead name": ({"0x35": {"levels": [{"dead": "Grave!"}]}},
                          {"dead": {"Grave!": {}}}),
            "composition": ({"0x35": {"levels": [{"dead": "grave"}]}},
                            {"dead": {"grave": {"a": "ab"}}}),
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
                self.assertEqual(data["source"]["libx11"], generator.REQUIRED_VERSIONS["libx11"])
                compiler.load(generator.LAYOUT_DIR, path.stem)

    def test_builtin_layouts_are_committed(self):
        committed = {path.stem for path in generator.LAYOUT_DIR.glob("*.json")}
        self.assertEqual(set(compiler.BUILTIN_LAYOUTS), committed)


class ToolchainVersionTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        (self.directory / "lib").mkdir()
        (self.directory / "lib/libxkbcommon.so").write_text("")
        (self.directory / "xkb/symbols").mkdir(parents=True)
        self.compose = self.directory / "share/X11/locale/en_US.UTF-8/Compose"
        self.compose.parent.mkdir(parents=True)
        self.compose.write_text("")

    def write_pc(self, xkbcommon, xkb_data, x11=None):
        x11 = x11 or generator.REQUIRED_VERSIONS["libx11"]
        (self.directory / "xkbcommon.pc").write_text(
            f"prefix={self.directory}\nlibdir=${{prefix}}/lib\n"
            f"Name: xkbcommon\nVersion: {xkbcommon}\n")
        (self.directory / "xkeyboard-config.pc").write_text(
            f"xkb_base={self.directory}/xkb\nName: XKeyboardConfig\nVersion: {xkb_data}\n")
        (self.directory / "x11.pc").write_text(
            f"prefix={self.directory}\nName: X11\nVersion: {x11}\n")

    def find(self):
        with mock.patch.dict(os.environ, {"PKG_CONFIG_PATH": str(self.directory)}), \
                mock.patch.object(generator, "PKG_CONFIG_DIRS", ()):
            return generator.find_toolchain()

    def test_pinned_versions_are_accepted(self):
        self.write_pc(generator.REQUIRED_VERSIONS["libxkbcommon"],
                      generator.REQUIRED_VERSIONS["xkeyboard-config"])
        library, root, compose = self.find()
        self.assertEqual(library, self.directory / "lib/libxkbcommon.so")
        self.assertEqual(root, self.directory / "xkb")
        self.assertEqual(compose, self.compose)

    def test_other_versions_are_refused(self):
        for xkbcommon, xkb_data in (("1.6.0", generator.REQUIRED_VERSIONS["xkeyboard-config"]),
                                    (generator.REQUIRED_VERSIONS["libxkbcommon"], "2.41")):
            with self.subTest(xkbcommon=xkbcommon, xkb_data=xkb_data):
                self.write_pc(xkbcommon, xkb_data)
                with self.assertRaisesRegex(generator.ToolchainError, "found, but"):
                    self.find()
        self.write_pc(generator.REQUIRED_VERSIONS["libxkbcommon"],
                      generator.REQUIRED_VERSIONS["xkeyboard-config"], x11="1.8.7")
        with self.assertRaisesRegex(generator.ToolchainError, "libx11 1.8.7 found"):
            self.find()


def pinned_toolchain():
    try:
        return generator.find_toolchain()
    except generator.ToolchainError as error:
        return str(error)


TOOLCHAIN = pinned_toolchain()


@unittest.skipIf(isinstance(TOOLCHAIN, str), f"pinned XKB toolchain unavailable: {TOOLCHAIN}")
class GenerateKeyboardLayoutsTest(unittest.TestCase):
    def test_compose_table_has_two_key_dead_sequences(self):
        library, _, compose_path = TOOLCHAIN
        compose = generator.read_compose(generator.load_xkbcommon(library), compose_path)
        self.assertEqual(compose["circumflex"]["e"], "ê")
        self.assertEqual(compose["circumflex"][" "], "^")
        self.assertEqual(compose["acute"][" "], "'")
        self.assertEqual(compose["diaeresis"][" "], "\"")
        # Longer sequences (dead_circumflex dead_acute a) are not dead key pairs.
        self.assertNotIn("dead_acute", compose["circumflex"])

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
