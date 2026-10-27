from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class OtaFlavorAliasesTest(unittest.TestCase):
    def test_signed_index_selection(self):
        source = (ROOT / "src/services/solar_os_ota.c").read_text()
        functions = production_functions(source, (
            "ota_flavor_is_valid", "ota_find_artifact_in_index"))
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "ota_selection.inc").write_text(functions)
            binary = Path(tmp, "aliases")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", tmp, str(ROOT / "tests/host/ota_flavor_aliases_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
