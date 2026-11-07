"""Exercise the installed Espressif scanner, including the SolarOS overlay."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from patch_keyboard_button import patch
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]
COMPONENT = ROOT / "managed_components/espressif__keyboard_button"


@unittest.skipUnless(COMPONENT.exists(), "install pinned keyboard_button with a firmware build first")
class MatrixComponentTest(unittest.TestCase):
    def test_overlay_and_full_matrix_release(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            component = directory / "component"
            shutil.copytree(COMPONENT, component)
            patch(component)
            before = {p.relative_to(component): p.read_bytes() for p in component.rglob("*") if p.is_file()}
            patch(component)
            self.assertEqual(before, {p.relative_to(component): p.read_bytes() for p in component.rglob("*") if p.is_file()})
            source = (component / "keyboard_button.c").read_text()
            typedefs = re.search(r"typedef struct \{.*?\} keyboard_btn_t;", source, re.S)
            self.assertIsNotNone(typedefs)
            harness = (ROOT / "tests/host/gpio_matrix_component_test.inc").read_text()
            harness = harness.replace("/* TYPES */", typedefs.group())
            harness = harness.replace("/* SCANNER */", production_functions(source, ("kbd_handler",)))
            harness = harness.replace("/* GPIO_INIT */", production_functions(
                (component / "src/kbd_gpio.c").read_text(), ("kbd_gpio_init",)))
            path = directory / "test.c"
            path.write_text(harness)
            binary = directory / "test"
            command = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wno-sign-compare",
                       "-fsanitize=address,undefined", "-fno-pie", "-no-pie"]
            for include in (component / "include", ROOT / "tests/host/inputronic_stubs", ROOT / "tests/host"):
                command += ["-I", str(include)]
            result = subprocess.run(command + [str(path), "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True)
            (component / "src/kbd_gpio.c").write_text("unexpected source version")
            with self.assertRaises(ValueError):
                patch(component)


if __name__ == "__main__":
    unittest.main()
