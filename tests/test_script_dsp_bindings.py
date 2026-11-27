"""Run the DSP bindings with the actual vendored language runtimes and ABI."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ScriptDspBindingsTest(unittest.TestCase):
    def run_runtime(self, language):
        command = ["cc", "-std=gnu11", "-O1", "-g", "-pthread", "-DSOLAR_OS_DSP_HOST_TEST",
            "-DSOLAR_OS_PACKAGE_SERVICE_DSP=1", "-include", str(ROOT / "tests/host/compat.h")]
        includes = [ROOT / "tests/host/native_abi_stubs", ROOT / "tests/host", ROOT / "include", ROOT / "src/services"]
        sources = [ROOT / "tests/host" / ("dsp_" + language + "_bindings_test.c")]
        sources += [ROOT / "src/services" / name for name in
            ("solar_os_native_dsp.c", "solar_os_native_handles.c", "solar_os_dsp.c")]
        if language == "lua":
            runtime = ROOT / "components/lua/lua/src"
            includes.append(runtime)
            sources += [path for path in runtime.glob("*.c") if path.name not in ("lua.c", "luac.c")]
            command += ["-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-fno-pie", "-no-pie"]
        else:
            component = ROOT / "components/micropython_embed"
            runtime = component / "micropython_embed"
            includes += [component, runtime]
            for directory in ("py", "extmod", "shared/runtime"):
                sources += list((runtime / directory).glob("*.c"))
            # The real VM's conservative GC scans its C stack. ASan would
            # reject that intentional scan; the adapter/kernel tests use ASan.
        for directory in includes:
            command += ["-I", str(directory)]
        with tempfile.TemporaryDirectory() as temporary:
            binary = Path(temporary) / language
            compiled = subprocess.run(command + list(map(str, sources)) + ["-lm", "-ldl", "-o", str(binary)],
                capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([str(binary)], check=True, timeout=20)

    def test_lua_with_real_interpreter(self):
        self.run_runtime("lua")

    def test_python_with_real_vm_and_gc(self):
        self.run_runtime("python")

    def test_language_bindings_depend_only_on_the_public_dsp_contract(self):
        for language in ("python", "lua"):
            source = (ROOT / "src/apps" / ("solar_os_" + language + "_dsp.inc")).read_text()
            self.assertFalse(re.search(r"solar_os_dsp_\w+\s*\(", source), language)
            self.assertNotIn("esp_err_t", source)
            self.assertNotIn("ESP_", source)
            self.assertNotRegex(source, r"solar_os_dsp_(?:fir|decimator|fft)_t")


if __name__ == "__main__":
    unittest.main()
