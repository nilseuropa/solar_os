from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class RadioGainTest(unittest.TestCase):
    def compile_run(self, source, *defines, extra_sources=()):
        with tempfile.TemporaryDirectory() as tmp:
            binary = Path(tmp, "radio-gain")
            subprocess.run([
                "cc", "-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
                "-D_POSIX_C_SOURCE=200809L", *defines,
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I", str(ROOT / "tests/host/gpio_controller_stubs"),
                "-I", str(ROOT / "tests/host/inputronic_stubs"),
                "-I", str(ROOT / "tests/host/expansion_stubs"),
                "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src"),
                "-I", str(ROOT / "src/drivers"),
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host" / source), *map(str, extra_sources), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary), tmp], check=True)

    def test_rfm69_lna_gain(self):
        self.compile_run("radio_gain_driver_test.c", "-DTEST_RFM69")

    def test_rfm95_lna_boost(self):
        self.compile_run("radio_gain_driver_test.c")

    def test_service_and_saved_profiles(self):
        self.compile_run("radio_gain_service_test.c", "-DSOLAR_OS_PACKAGE_SERVICE_RADIO=1",
                         extra_sources=[ROOT / "src/services" / source for source in
                                        ("solar_os_native_radio.c", "solar_os_native_handles.c",
                                         "solar_os_native_device_providers.c")])
