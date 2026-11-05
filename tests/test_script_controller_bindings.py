from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class ControllerBindingsTest(unittest.TestCase):
    def run_bindings(self, runtime):
        lua_dir = ROOT / "components/lua/lua/src"
        sources = [ROOT / "src/services" / name for name in (
            "solar_os_controllers.c", "solar_os_controller_math.c", "solar_os_controller_tuning.c", "solar_os_parameters.c", "solar_os_stream.c", "solar_os_motor.c", "solar_os_encoder.c")]
        if runtime == "lua":
            sources += [p for p in lua_dir.glob("*.c") if p.name not in {"lua.c", "luac.c"}]
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "test"
            fixture = ROOT / f"tests/host/controllers_{runtime}_bindings_test.inc"
            if runtime == "shell":
                source = (ROOT / "src/shell/solar_os_shell_controllers.c").read_text()
                code = re.search(r'static const char usage\[\] =.*?;', source, re.S).group()
                code += "\n" + production_functions(source, ("number", "print_info", "solar_os_shell_cmd_controller"))
                fixture = Path(directory) / "shell.c"
                fixture.write_text((ROOT / "tests/host/controllers_shell_bindings_test.inc").read_text()
                                   .replace("/* SHELL_COMMAND */", code))
            command = ["cc", "-x", "c", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                       "-include", str(ROOT / "tests/host/compat.h")]
            for path in (ROOT / "tests/host/pwm_stubs", ROOT / "tests/host", ROOT / "include", ROOT / "src/services",
                         ROOT / "src/apps", ROOT / "src", lua_dir):
                command += ["-I", str(path)]
            command += [str(fixture)]
            command += [str(p) for p in sources]
            result = subprocess.run(command + ["-lm", "-ldl", "-o", str(target)],
                                    capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(target)], check=True)

    def test_python_wrapper_behavior(self):
        self.run_bindings("python")

    def test_lua_with_real_interpreter(self):
        self.run_bindings("lua")

    def test_shell_tuning_routing_and_ownership(self):
        self.run_bindings("shell")
