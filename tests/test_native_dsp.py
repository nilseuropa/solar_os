from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeDspTest(unittest.TestCase):
    def test_real_kernels_and_typed_resource_lifetimes(self):
        for enabled in (0, 1):
            with self.subTest(enabled=enabled), tempfile.TemporaryDirectory() as temporary:
                binary = Path(temporary) / "dsp"
                subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread",
                    "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DSOLAR_OS_DSP_HOST_TEST",
                    "-DSOLAR_OS_PACKAGE_SERVICE_DSP=" + str(enabled),
                    "-DSOLAR_OS_PACKAGE_SERVICE_SIGNAL_WIDGETS=" + str(enabled),
                    "-I" + str(ROOT / "tests/host/pwm_stubs"),
                    "-I" + str(ROOT / "tests/host/native_abi_stubs"), "-I" + str(ROOT / "tests/host"),
                    "-I" + str(ROOT / "include"), "-I" + str(ROOT / "src"), "-I" + str(ROOT / "src/services"),
                    str(ROOT / "tests/host/native_dsp_test.c"),
                    *(str(ROOT / "src/services" / name) for name in
                      ("solar_os_native_dsp.c", "solar_os_native_handles.c", "solar_os_dsp.c",
                       "solar_os_native_media_widgets.c", "solar_os_signal_widgets.c")),
                    "-Wl,--wrap=calloc,--wrap=free", "-lm", "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
