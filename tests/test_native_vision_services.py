from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


class NativeVisionServicesTest(unittest.TestCase):
    def test_public_headers_compile_without_sdk_in_c_and_cpp(self):
        headers = ["solar_os_native_abi.h", "solar_os_native_job_abi.h",
                   "solar_os_native_image_abi.h", "solar_os_native_media_abi.h",
                   "solar_os_native_vision_abi.h", "solar_os_native_inference_abi.h"]
        code = "".join(f'#include "{name}"\n' for name in headers)
        with tempfile.TemporaryDirectory() as tmp:
            for compiler, suffix, standard in (("cc", "c", "c11"), ("c++", "cpp", "c++17")):
                path = Path(tmp) / ("client." + suffix)
                path.write_text(code)
                subprocess.run([compiler, "-std="+standard, "-Wall", "-Wextra", "-Werror",
                                "-pedantic", "-I", str(ROOT / "include"), "-fsyntax-only", str(path)], check=True)

    def test_disabled_bridge_compiles_without_backends(self):
        with tempfile.TemporaryDirectory() as tmp:
            source = Path(tmp) / "disabled.c"
            source.write_text('#include <assert.h>\n#include "solar_os_native_media.h"\n'
                              '#include "solar_os_memory.h"\n'
                              'void *solar_os_memory_alloc(size_t n, solar_os_memory_class_t k, const char *t) { (void)n; (void)k; (void)t; return 0; }\n'
                              'void solar_os_memory_free(void *p) { (void)p; }\n'
                              'int main(void) { assert(!solar_os_native_media_get_service("image", 1, 0)); }\n')
            binary = Path(tmp) / "disabled"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "include"),
                            "-I", str(ROOT / "tests/host/rtsp_stubs"), "-I", str(ROOT / "src"),
                            "-I", str(ROOT / "src/services"), str(source),
                            str(ROOT / "src/services/solar_os_native_media.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_application_host_extension_preserves_original_prefix(self):
        header = (ROOT / "include/solar_os_native_abi.h").read_text()
        self.assertGreater(header.index("get_service"), header.index("write_utf8"))
        native = (ROOT / "src/services/solar_os_native.c").read_text()
        self.assertIn(".get_service = native_job_get_service", native)
        self.assertIn("solar_os_native_media_get_service(name", native)

    def test_acceptance_module_declares_extended_host_requirement(self):
        manifest = tomllib.loads((ROOT / "modules/vision-check/module.toml").read_text())
        self.assertEqual(manifest["native_abi"], 1)
        self.assertEqual(manifest["minimum_host_api_size"], 24)
        source = (ROOT / "modules/vision-check/main/vision_check.c").read_text()
        self.assertIn("solar_os_native_host_v1()", source)
        self.assertNotIn('#include "esp_', source)


if __name__ == "__main__":
    unittest.main()
