from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class GpioIsrTest(unittest.TestCase):
    def test_shared_install_retry_and_concurrent_callers(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, "gpio_isr")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(ROOT / "tests/host/gpio_controller_stubs"),
                "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/drivers"),
                str(ROOT / "tests/host/gpio_isr_test.c"),
                str(ROOT / "src/drivers/solar_os_gpio_isr.c"),
                "-o", str(binary),
            ], check=True)
            for mode in range(3):
                with self.subTest(mode=mode):
                    subprocess.run([str(binary), str(mode)], check=True)


if __name__ == "__main__":
    unittest.main()
