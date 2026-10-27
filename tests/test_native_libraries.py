from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class NativeLibrariesTest(unittest.TestCase):
    def test_runtime_lifecycle_and_concurrent_leases(self):
        source = (ROOT / "src/services/solar_os_native.c").read_text()
        structures = "\n".join(re.search(
            rf"typedef struct {name} \{{.*?\}} {name}_t;", source, re.S).group()
            for name in ("native_resident_slot", "native_library_lease"))
        functions = production_functions(source, (
            "native_descriptor_text_valid", "native_register_library", "native_library_result",
            "native_library_link_lease", "native_library_acquire_loaded", "native_library_id_valid",
            "solar_os_native_library_acquire", "solar_os_native_library_retain",
            "solar_os_native_library_release", "solar_os_native_library_unload",
            "native_reclaim_unused_libraries",
            "native_find_slot", "native_slot_count", "native_slot_add", "native_slot_remove",
            "native_unregister_resident", "solar_os_native_module_activate",
            "solar_os_native_module_deactivate", "solar_os_native_module_active"))
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "library_types.inc").write_text(structures)
            Path(tmp, "library_runtime.inc").write_text(functions)
            binary = Path(tmp, "libraries")
            subprocess.run([
                "cc", "-D_POSIX_C_SOURCE=200809L", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-pthread", "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "include"), "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/native_libraries_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

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
