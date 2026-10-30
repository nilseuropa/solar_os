from pathlib import Path
import json
import sys
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
from check_memory_contracts import inventory, sites


class MemoryContractsTest(unittest.TestCase):
    def test_all_current_packages_and_native_ports_are_inventoried(self):
        report, errors = inventory(ROOT)
        self.assertEqual(errors, [])
        catalog = tomllib.loads((ROOT / "packages/solar_os_packages.toml").read_text())
        self.assertEqual(set(report["packages"]), set(catalog["packages"]))
        for path in ("src/apps/solar_os_reader.c", "src/apps/solar_os_writer.c",
                     "src/services/solar_os_ble_nimble_server.inc", "modules/doom/main/port.c"):
            self.assertIn(path, report["files"])
        self.assertIn("sdk", report["boundaries"])

    def test_existing_files_and_new_features_cannot_add_raw_allocations_silently(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "packages").mkdir()
            (root / "src").mkdir()
            (root / "packages/solar_os_packages.toml").write_text(
                '[packages.feature]\nsources = ["feature.c"]\n')
            contracts = {"exceptions": {}, "boundaries": {}}
            config = root / "packages/memory_contracts.json"
            config.write_text(json.dumps(contracts))
            feature = root / "src/feature.c"
            feature.write_text('/* malloc(42); */\nconst char *s = "calloc(1,2)";\n'
                               'void *p = solar_os_memory_alloc(42, cls, "feature");\n')
            self.assertEqual(len(sites(feature)), 1)
            self.assertEqual(inventory(root)[1], [])
            feature.write_text('void *p = malloc(42);\n')
            self.assertIn("raw allocation contract changed", inventory(root)[1][0])
            contracts["exceptions"]["src/feature.c"] = {
                "calls": {"malloc": 1}, "reason": "SDK borrowed pointer lifetime"}
            config.write_text(json.dumps(contracts))
            self.assertEqual(inventory(root)[1], [])
            feature.write_text('void *p = malloc(42); void *q = malloc(50);\n')
            self.assertTrue(inventory(root)[1])
