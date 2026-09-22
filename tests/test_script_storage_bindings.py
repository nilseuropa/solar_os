import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DESCRIPTOR = (ROOT / "src/apps/solar_os_script_api.inc").read_text(encoding="utf-8")
HEADER = (ROOT / "src/services/solar_os_storage.h").read_text(encoding="utf-8")
SOURCE = (ROOT / "src/services/solar_os_storage.c").read_text(encoding="utf-8")
PYTHON = (ROOT / "src/apps/solar_os_python.c").read_text(encoding="utf-8")
LUA = (ROOT / "src/apps/solar_os_lua.c").read_text(encoding="utf-8")


class ScriptStorageBindingsTest(unittest.TestCase):
    def test_discovery_methods_have_python_lua_parity(self):
        for method in ("stat", "exists", "scandir", "makedirs"):
            self.assertIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(storage, {method}, {method});",
                DESCRIPTOR,
            )
            self.assertIn(f"solaros_storage_{method}_obj", PYTHON)
            self.assertIn(f"solua_storage_{method}", LUA)

    def test_scandir_is_service_owned_and_bounded(self):
        self.assertIn("SOLAR_OS_STORAGE_SCANDIR_MAX_LIMIT 128U", HEADER)
        self.assertIn("solar_os_storage_scandir", HEADER)
        self.assertIn("DIR *directory = opendir(path);", SOURCE)
        self.assertIn("limit > SOLAR_OS_STORAGE_SCANDIR_MAX_LIMIT", SOURCE)
        self.assertIn('\"python.scandir\"', PYTHON)
        self.assertIn('\"lua.scandir\"', LUA)
        self.assertIn('python_key(\"next_cursor\")', PYTHON)
        self.assertIn('\"next_cursor\"', LUA)

    def test_metadata_fields_match(self):
        for field in ("type", "is_file", "is_dir", "size", "mtime", "mode"):
            self.assertIn(f'\"{field}\"', PYTHON)
            self.assertIn(f'\"{field}\"', LUA)


if __name__ == "__main__":
    unittest.main()
