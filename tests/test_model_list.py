from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class ModelListTest(unittest.TestCase):
    def test_independent_backends_discovery_busy_errors_and_legacy_handles(self):
        code = production_functions((ROOT / "src/shell/solar_os_shell_model.c").read_text(), (
            "list_resident_models", "list_camera_models"))
        fixture = (ROOT / "tests/host/model_list_test.inc").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp, "test.c")
            source.write_text(fixture.replace("/* PRODUCTION */", code))
            binary = Path(tmp, "model-list")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-no-pie",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src/services"),
                str(source), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
