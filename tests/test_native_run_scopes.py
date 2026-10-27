from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class NativeRunScopesTest(unittest.TestCase):
    def test_app_manifest_has_the_same_layout_and_export_in_c_and_cpp(self):
        code = ('#include <stddef.h>\n#include "solar_os_native_app_abi.h"\n'
                'typedef char size_check[sizeof(solar_os_native_app_manifest_v2_t)==232?1:-1];\n'
                'typedef char id_check[offsetof(solar_os_native_app_manifest_v2_t,id)==40?1:-1];\n'
                'typedef char ext_check[offsetof(solar_os_native_app_manifest_v2_t,file_extensions)==168?1:-1];\n'
                'SOLAR_OS_NATIVE_APP_MANIFEST = {0};\n')
        with tempfile.TemporaryDirectory() as tmp:
            for compiler, language in (("cc", "c"), ("c++", "c++")):
                binary = Path(tmp, compiler + ".o")
                subprocess.run([compiler, "-x", language, "-fvisibility=hidden",
                                "-I", str(ROOT / "include"), "-c", "-o", str(binary), "-"],
                               input=code, text=True, check=True)
                symbols = subprocess.check_output(["nm", "-g", str(binary)], text=True)
                self.assertRegex(symbols, r"\bR solar_os_native_app_manifest\b")

    def test_interleaved_modules_keep_their_own_services_and_cleanup_before_unload(self):
        source = (ROOT / "src/services/solar_os_native.c").read_text()
        scope = re.search(r"typedef struct native_run_scope \{.*?\} native_run_scope_t;", source, re.S).group()
        functions = production_functions(source, (
            "native_run_scope_current", "native_run_scope_add", "native_run_scope_remove",
            "solar_os_native_run_service_user", "native_write_utf8", "native_job_get_service",
            "solar_os_native_run"))
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "native_run.inc").write_text(scope + "\n" + functions)
            binary = Path(tmp, "scopes")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", tmp,
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "include"),
                "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/native_run_scopes_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
