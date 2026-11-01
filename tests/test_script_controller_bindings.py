from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class ControllerBindingsTest(unittest.TestCase):
    def run_bindings(self, runtime):
        lua_dir = ROOT / "components/lua/lua/src"
        sources = [ROOT / "src/services" / name for name in (
            "solar_os_controllers.c", "solar_os_controller_math.c", "solar_os_parameters.c")]
        if runtime == "lua":
            sources += [p for p in lua_dir.glob("*.c") if p.name not in {"lua.c", "luac.c"}]
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "test"
            command = ["cc", "-x", "c", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                       "-include", str(ROOT / "tests/host/compat.h")]
            for path in (ROOT / "tests/host", ROOT / "include", ROOT / "src/services",
                         ROOT / "src/apps", lua_dir):
                command += ["-I", str(path)]
            command += [str(ROOT / f"tests/host/controllers_{runtime}_bindings_test.inc")]
            command += [str(p) for p in sources]
            result = subprocess.run(command + ["-lm", "-ldl", "-o", str(target)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(target)], check=True)

    def test_python_wrapper_behavior(self):
        self.run_bindings("python")

    def test_lua_with_real_interpreter(self):
        self.run_bindings("lua")
