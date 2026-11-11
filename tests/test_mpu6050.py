from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class MPU6050Test(unittest.TestCase):
    def compile_and_run(self, fixture, service=False):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            compat = tmp / "compat.h"
            compat.write_text('#include "freertos/semphr.h"\n#include <stddef.h>\n'
                'size_t strlcpy(char *, const char *, size_t);\n'
                'void vSemaphoreDelete(SemaphoreHandle_t);\n')
            args = ["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined"]
            if service:
                args += ["-include", str(compat), "-I", str(ROOT / "tests/host/gpio_controller_stubs")]
            for path in ("tests/host", "tests/host/expansion_stubs", "src", "src/services", "src/drivers"):
                args += ["-I", str(ROOT / path)]
            args += ["-idirafter", str(ROOT / "include"), str(ROOT / "tests/host" / fixture),
                     str(ROOT / "src/drivers/mpu6050.c"), "-lm", "-o", str(tmp / "test")]
            subprocess.run(args, check=True)
            subprocess.run([str(tmp / "test")], check=True)

    def test_registers_units_reset_and_cleanup(self):
        self.compile_and_run("mpu6050_test.c")

    def test_provider_polling_optional_irq_timeout_and_detach(self):
        self.compile_and_run("mpu6050_service_test.c", service=True)
