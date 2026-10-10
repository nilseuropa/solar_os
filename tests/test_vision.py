import ast
import re
import sys
import tomllib
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import generate_flavor_config as flavors


class VisionIntegrationTest(unittest.TestCase):
    def test_vision_prunes_without_psram_and_does_not_require_camera(self):
        catalog = flavors.load_catalog(ROOT / "packages/solar_os_packages.toml")
        group_values = {group: group == "vision" for group in catalog.groups}
        packages = {name: False for name in catalog.packages}
        selected = flavors.enable_required_packages(catalog, packages, {"service_vision"})
        _, enabled = flavors.apply_board_capability_pruning(catalog, group_values, selected, {"psram"})
        self.assertTrue(enabled["service_vision"])
        self.assertTrue(enabled["service_image"])
        self.assertFalse(enabled["service_camera"])
        _, disabled = flavors.apply_board_capability_pruning(catalog, group_values, selected, set())
        self.assertFalse(disabled["service_vision"])

    def test_optional_flavor_and_service_boundaries(self):
        for flavor in ("core", "rover", "netrunner", "writerdeck", "full"):
            data = tomllib.loads((ROOT / f"flavors/{flavor}.toml").read_text())
            self.assertEqual(data["groups"].get("vision", False), flavor == "full")
        source = (ROOT / "src/services/solar_os_vision.c").read_text()
        self.assertNotRegex(source, r"\b(?:esp_camera|mp_obj|lua)_\w+")

    def test_documented_python_examples_are_valid(self):
        page = (ROOT / "doc/manual/vision.md").read_text()
        for example in re.findall(r"```python\n(.*?)```", page, re.S):
            ast.parse(example)


if __name__ == "__main__":
    unittest.main()
