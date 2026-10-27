from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class EncoderBindingsTest(unittest.TestCase):
    def run_bindings(self, runtime):
        source = (ROOT / f"src/apps/solar_os_{runtime}.c").read_text()
        if runtime == "python":
            functions = ("python_encoder_info",) + tuple(f"solaros_encoder_{name}" for name in
                         ("list", "status", "read", "reset")) + (
                         "python_expansion_key_known", "python_expansion_validate_keys",
                         "python_expansion_add_binding", "solaros_expansion_attach")
        else:
            functions = ("solua_push_encoder_info",) + tuple(f"solua_encoder_{name}" for name in
                         ("list", "status", "read", "reset")) + (
                         "solua_expansion_key_known", "solua_expansion_validate_keys",
                         "solua_table_optional_string", "solua_table_optional_int",
                         "solua_expansion_add_binding", "solua_expansion_attach")
        code = (ROOT / f"tests/host/encoder_{runtime}_bindings_test.inc").read_text()
        code = code.replace("/* ENCODER_BINDINGS */", production_functions(source, functions))
        code = code.replace("/* ENCODER_COMPLETION */", production_functions(
            (ROOT / "src/apps/solar_os_shell.c").read_text(), ("shell_completion_emit_encoder_names",)))
        lua_dir = ROOT / "components/lua/lua/src"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(code)
            command = ["cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                       "-include", str(ROOT / "tests/host/compat.h")]
            for include in ("tests/host/pwm_stubs", "tests/host/expansion_stubs", "tests/host",
                            "include", "src", "src/services", "components/lua/lua/src"):
                command += ["-I", str(ROOT / include)]
            command += [str(path / "test.c"), str(ROOT / "src/services/solar_os_encoder.c")]
            if runtime == "lua":
                command += [str(p) for p in lua_dir.glob("*.c") if p.name not in {"lua.c", "luac.c"}]
            try:
                subprocess.run(command + ["-pthread", "-lm", "-ldl", "-o", str(path / "test")],
                               check=True, capture_output=True, text=True)
            except subprocess.CalledProcessError as error:
                self.fail(error.stderr)
            subprocess.run([str(path / "test")], check=True)

    def test_python_records_counts_and_expansion_encoding(self):
        self.run_bindings("python")

    def test_lua_records_counts_and_expansion_encoding(self):
        self.run_bindings("lua")


if __name__ == "__main__":
    unittest.main()
