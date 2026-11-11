from pathlib import Path
import subprocess
import re
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class FlashStorageTest(unittest.TestCase):
    @staticmethod
    def function(source, name):
        match = re.search(r"^(?:static )?(?:esp_err_t|bool|void)\s+" + name + r"\([^;]*?\)\s*\{", source, re.M)
        assert match, name
        end = source.index("{", match.start()) + 1
        depth = 1
        while depth:
            depth += (source[end] == "{") - (source[end] == "}")
            end += 1
        return source[match.start():end] + "\n"

    def test_runtime_cache_selection_and_leases(self):
        with tempfile.TemporaryDirectory() as directory:
            service = (ROOT / "src/services/solar_os_flash.c").read_text()
            code = (ROOT / "tests/host/flash_cache_operations.inc").read_text()
            for name in ("flash_name_valid", "flash_join", "flash_remove_tree", "solar_os_flash_catalog_free",
                         "solar_os_flash_catalog_load_at", "solar_os_flash_catalog_load",
                         "solar_os_flash_catalog_refresh_at", "solar_os_flash_catalog_refresh", "solar_os_flash_artifact_delete"):
                code += self.function(service, name)
            Path(directory, "flash_cache_generated.inc").write_text(code)
            binary = Path(directory, "flash_cache")
            subprocess.run([
                "cc", "-std=gnu11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I" + directory,
                "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src/services"),
                str(ROOT / "tests/host/flash_cache_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == "__main__":
    unittest.main()
