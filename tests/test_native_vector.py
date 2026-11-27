from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeVectorTest(unittest.TestCase):
    def test_real_gif_decoder_matches_both_abi_callbacks_on_a_small_stack(self):
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / "gif"
            sources = [ROOT / "tests/host/gif_decode_stack_test.c",
                ROOT / "src/services/solar_os_native_vector.c", ROOT / "src/services/solar_os_vector.c"]
            sources += [ROOT / "components/stb_image" / name for name in
                ("stb_image_port.c", "jpeg_fast.c", "jpeg_simd.c")]
            subprocess.run(["cc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
                "-DSOLAR_OS_TEST_VECTOR_ABI=1", "-DSOLAR_OS_PACKAGE_SERVICE_VECTOR=1",
                "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "include"),
                "-I" + str(ROOT / "src/services"), "-I" + str(ROOT / "components/stb_image/include"),
                *map(str, sources), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], cwd=ROOT / "tests/host", check=True)

    def test_bulk_conversions_use_the_existing_kernels(self):
        for enabled in (0, 1):
            with self.subTest(enabled=enabled), tempfile.TemporaryDirectory() as temporary:
                binary = Path(temporary) / "vector"
                subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-pie", "-no-pie",
                    "-DSOLAR_OS_PACKAGE_SERVICE_VECTOR=" + str(enabled),
                    "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "include"),
                    "-I" + str(ROOT / "src/services"),
                    str(ROOT / "tests/host/native_vector_test.c"),
                    str(ROOT / "src/services/solar_os_native_vector.c"),
                    str(ROOT / "src/services/solar_os_vector.c"), "-o", str(binary)], check=True)
                subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
