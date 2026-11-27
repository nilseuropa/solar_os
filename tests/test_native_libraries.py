from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


def write_runtime_support(stage):
    (stage / 'freertos').mkdir()
    (stage / 'solar_os_config.h').write_text(
        '#define SOLAR_OS_PACKAGE_SERVICE_NATIVE_MODULES 1\n'
        '#define SOLAR_OS_PACKAGE_SERVICE_EXPANSION 0\n')
    (stage / 'freertos/FreeRTOS.h').write_text(
        f'#include "{ROOT}/tests/host/native_abi_stubs/freertos/FreeRTOS.h"\n')
    (stage / 'freertos/task.h').write_text(
        '#pragma once\n#include <sched.h>\ntypedef void *TaskHandle_t;\n'
        'extern _Thread_local int solar_test_task_identity;\n'
        'static inline TaskHandle_t xTaskGetCurrentTaskHandle(void) { return &solar_test_task_identity; }\n'
        'static inline void vTaskDelay(int n) { (void)n; sched_yield(); }\n')


class NativeLibrariesTest(unittest.TestCase):
    def test_runtime_lifecycle_and_concurrent_leases(self):
        with tempfile.TemporaryDirectory() as tmp:
            stage = Path(tmp)
            write_runtime_support(stage)
            binary = stage / 'libraries'
            subprocess.run([
                "cc", "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "include"), "-I", str(ROOT / "src"), "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/native_libraries_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True, timeout=30)

    def test_library_abi_is_usable_from_c_and_cpp(self):
        with tempfile.TemporaryDirectory() as tmp:
            for compiler, language in (("cc", "c"), ("c++", "c++")):
                subprocess.run([compiler, "-x", language, "-Wall", "-Wextra", "-Werror",
                    "-I", str(ROOT / "include"), "-c", "-o", str(Path(tmp, compiler + ".o")), "-"],
                    input='#include "solar_os_native_library_abi.h"\n'
                          'const solar_os_native_library_host_api_v1_t *test(void) { '
                          'return solar_os_native_library_host_v1(); }\n',
                    text=True, check=True)


if __name__ == "__main__":
    unittest.main()
