from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


class LSM6DS3TRCTest(unittest.TestCase):
    def test_registers_units_and_failure_cleanup(self):
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory, "imu")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/drivers"), str(ROOT / "tests/host/lsm6ds3trc_test.c"),
                str(ROOT / "src/drivers/lsm6ds3trc.c"), "-lm", "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_sticky_owns_imu_on_sensor_bus(self):
        board = tomllib.loads((ROOT / "boards/manifests/seeed_reterminal_sticky.toml").read_text())
        imu = next(device for device in board["devices"] if device["name"] == "imu0")
        self.assertEqual(imu["driver"], "lsm6ds3tr-c")
        self.assertEqual(imu["bindings"], {"i2c": "i2c0", "addr": 0x6a})

    def test_provider_timeout_registration_and_detach(self):
        with tempfile.TemporaryDirectory() as directory:
            tmp = Path(directory)
            compat = tmp / "compat.h"
            compat.write_text('#include "freertos/semphr.h"\n#include <stddef.h>\n'
                'size_t strlcpy(char *, const char *, size_t);\n'
                'void vSemaphoreDelete(SemaphoreHandle_t);\n')
            binary = tmp / "provider"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-include", str(compat),
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "tests/host/expansion_stubs"),
                "-I", str(ROOT / "src"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src/drivers"), "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/lsm6ds3trc_service_test.c"),
                str(ROOT / "src/services/solar_os_lsm6ds3trc.c"),
                str(ROOT / "src/drivers/lsm6ds3trc.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
