from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class MotorBindingsTest(unittest.TestCase):
    def run_bindings(self, runtime):
        source = (ROOT / f"src/apps/solar_os_{runtime}.c").read_text()
        functions = (("python_motor_info",) + tuple(f"solaros_motor_{name}" for name in
                     ("list", "status", "drive", "coast", "brake", "clear", "configure")) if runtime == "python" else
                     ("solua_push_motor_info",) + tuple(f"solua_motor_{name}" for name in
                     ("list", "status", "drive", "coast", "brake", "clear", "configure")))
        code = (ROOT / f"tests/host/motor_{runtime}_bindings_test.inc").read_text()
        code = code.replace("/* MOTOR_BINDINGS */", production_functions(source, functions))
        lua_dir = ROOT / "components/lua/lua/src"
        lua_sources = [str(p) for p in lua_dir.glob("*.c") if p.name not in {"lua.c", "luac.c"}]
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(code)
            command = ["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                       "-include", str(ROOT / "tests/host/compat.h"),
                       "-I", str(ROOT / "tests/host/pwm_stubs"), "-I", str(ROOT / "tests/host"),
                       "-I", str(ROOT / "include"), "-I", str(ROOT / "src"),
                       "-I", str(ROOT / "src/services"), "-I", str(lua_dir),
                       str(path / "test.c"), str(ROOT / "src/services/solar_os_motor.c"), str(ROOT / "src/services/solar_os_stream.c"), str(ROOT / "src/services/solar_os_parameters.c"), str(ROOT / "tests/host/motion_endpoint_clock.c")]
            if runtime == "lua":
                command += lua_sources
            result=subprocess.run(command + ["-pthread", "-lm", "-ldl", "-o", str(path / "test")],
                                  capture_output=True,text=True)
            self.assertEqual(result.returncode,0,result.stderr)
            subprocess.run([str(path / "test")], check=True)

    def test_python_records_normalized_commands_and_errors(self):
        self.run_bindings("python")

    def test_lua_records_normalized_commands_and_errors(self):
        self.run_bindings("lua")


if __name__ == "__main__":
    unittest.main()
