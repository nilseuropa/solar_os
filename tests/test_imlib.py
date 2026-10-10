import ast
from pathlib import Path
import re
import sys
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import generate_flavor_config as flavors


class ImlibIntegrationTest(unittest.TestCase):
    def test_independent_imlib_selection_on_classic_and_s3(self):
        catalog = flavors.load_catalog(ROOT / "packages/solar_os_packages.toml")
        groups = {name: False for name in catalog.groups}
        packages = {name: False for name in catalog.packages}
        selected = flavors.enable_required_packages(catalog, packages, {"service_imlib"})
        for target in ("esp32", "esp32s3"):
            _, enabled = flavors.apply_board_capability_pruning(catalog, groups, selected, {"psram"})
            enabled = flavors.apply_target_pruning(catalog, enabled, target)
            self.assertTrue(enabled["service_imlib"])
            self.assertTrue(enabled["service_image"])
            for unrelated in ("service_vision", "service_inference", "service_camera", "service_pipeline", "app_python", "app_lua"):
                self.assertFalse(enabled[unrelated], unrelated)
        _, disabled = flavors.apply_board_capability_pruning(catalog, groups, selected, set())
        self.assertFalse(disabled["service_imlib"])

    def test_documented_python_processing_examples_parse(self):
        text = (ROOT / "doc/manual/vision.md").read_text()
        for example in re.findall(r"```python\n(.*?)```", text, re.S):
            ast.parse(example)


if __name__ == "__main__":
    unittest.main()
