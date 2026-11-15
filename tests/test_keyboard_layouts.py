import json
from pathlib import Path
import sys
import tempfile
import unittest

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
                "source": {"xkeyboard-config": "test", "layout": stem, "variant": ""},
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
                self.assertEqual(set(data["source"]), {"xkeyboard-config", "layout", "variant"})
                compiler.load(generator.LAYOUT_DIR, path.stem)


@unittest.skipIf(generator.default_xkb_root() is None, "xkeyboard-config not installed")
class GenerateKeyboardLayoutsTest(unittest.TestCase):
    def test_committed_layouts_match_xkb(self):
        try:
            generator.load_xkbcommon()
        except SystemExit:
            self.skipTest("libxkbcommon not installed")
        root = generator.default_xkb_root()
        with tempfile.TemporaryDirectory() as temporary:
            for path in sorted(generator.LAYOUT_DIR.glob("*.json")):
                committed = json.loads(path.read_text(encoding="utf-8"))
                if committed["source"]["xkeyboard-config"] != generator.xkb_version(root):
                    continue
                (Path(temporary) / path.name).write_text(path.read_text(encoding="utf-8"))
            self.assertEqual(generator.main(["--all", "--output-dir", temporary]), 0)
            for path in sorted(Path(temporary).glob("*.json")):
                with self.subTest(path.name):
                    self.assertEqual(path.read_text(encoding="utf-8"),
                                     (generator.LAYOUT_DIR / path.name).read_text(encoding="utf-8"))


if __name__ == "__main__":
    unittest.main()
