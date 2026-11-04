from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class RobotBindingsTest(unittest.TestCase):
    def run_bindings(self, runtime):
        lua = ROOT / "components/lua/lua/src"
        sources = [ROOT / "src/services" / name for name in (
            "solar_os_controller_math.c", "solar_os_controllers.c", "solar_os_native_controllers.c", "solar_os_parameters.c", "solar_os_motor.c", "solar_os_encoder.c",
            "solar_os_native_motion.c", "solar_os_robot.c")]
        sources += [ROOT / "modules/robot/main" / name for name in ("model.c", "transform.c")]
        if runtime == "lua":
            sources += [p for p in lua.glob("*.c") if p.name not in {"lua.c", "luac.c"}]
        with tempfile.TemporaryDirectory() as directory:
            binary = Path(directory) / "test"
            fixture = ROOT / f"tests/host/robot_{runtime}_bindings_test.inc"
            if runtime == "shell":
                source = (ROOT / "src/shell/solar_os_shell_robot.c").read_text()
                usage = re.search(r'static const char usage\[\] =.*?;', source, re.S).group()
                usage += "\n" + re.search(r'static const char model_usage\[\] =.*?;', source, re.S).group()
                common = (ROOT / "src/shell/solar_os_shell_common.c").read_text()
                code = usage + "\n" + production_functions(common, ("solar_os_shell_parse_size_arg",))
                code += "\n" + production_functions(source, (
                    "number", "show", "joint_command", "model_command", "solar_os_shell_cmd_robot"))
                fixture = Path(directory) / "shell.c"
                fixture.write_text((ROOT / "tests/host/robot_shell_bindings_test.inc").read_text()
                                   .replace("/* SHELL_COMMAND */", code))
            cmd = ["cc", "-x", "c", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                   '-DROBOT_MODEL_FIXTURE="' + str(ROOT / "tests/fixtures/solbot.urdf") + '"',
                   "-Wno-unused-function", "-include", str(ROOT / "tests/host/compat.h")]
            for path in (ROOT / "tests/host/robot_stubs", ROOT / "tests/host/pwm_stubs",
                         ROOT / "tests/host", ROOT / "include", ROOT / "src", ROOT / "src/services", ROOT / "src/apps", lua):
                cmd += ["-I", str(path)]
            cmd += [str(fixture)] + [str(p) for p in sources]
            result = subprocess.run(cmd + ["-pthread", "-lm", "-ldl", "-o", str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(binary)], check=True)

    def test_python_wrappers(self):
        self.run_bindings("python")

    def test_lua_runtime(self):
        self.run_bindings("lua")

    def test_shell_joint_namespace_and_argument_routing(self):
        self.run_bindings("shell")
