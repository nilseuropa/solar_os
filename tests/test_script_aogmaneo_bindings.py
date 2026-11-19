from pathlib import Path
import ast
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class AOgmaNeoBindingsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="solaros-aogmaneo-bindings-")
        cls.output = Path(cls.temporary.name)
        cls.flags = ["-O1", "-g", "-fsanitize=address,undefined", "-fno-sanitize-recover=all",
                     "-fno-omit-frame-pointer", "-fno-pie"]
        cls.includes = ["-I" + str(ROOT / path) for path in (
            "include", "src/services", "src/apps", "tests/host", "modules/aogmaneo", "components/lua/lua/src")]
        cls.objects = []
        paths = [ROOT / "modules/aogmaneo/engine.cpp", *sorted((ROOT / "modules/aogmaneo/vendor/aogmaneo").glob("*.cpp")),
                 ROOT / "src/services/solar_os_aogmaneo.c", ROOT / "tests/host/aogmaneo_bindings_support.c"]
        for path in paths:
            obj = cls.output / (path.stem + ".o")
            compiler, standard = ("g++", "c++17") if path.suffix == ".cpp" else ("cc", "c11")
            subprocess.run([compiler, "-std=" + standard, *cls.flags, *cls.includes, "-DUSE_STD_MATH=1",
                            "-c", str(path), "-o", str(obj)], check=True)
            cls.objects.append(str(obj))

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def run_bindings(self, runtime):
        paths = [ROOT / f"tests/host/aogmaneo_{runtime}_bindings_test.c"]
        if runtime == "lua":
            paths += [p for p in (ROOT / "components/lua/lua/src").glob("*.c") if p.name not in {"lua.c", "luac.c"}]
        objects = list(self.objects)
        for path in paths:
            obj = self.output / (runtime + "-" + path.stem + ".o")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror", *self.flags, *self.includes,
                            "-c", str(path), "-o", str(obj)], check=True)
            objects.append(str(obj))
        binary = self.output / runtime
        subprocess.run(["g++", *self.flags, "-no-pie", *objects, "-lm", "-ldl", "-o", str(binary)], check=True)
        arguments = [str(binary)]
        if runtime == "lua":
            arguments.append(str(ROOT / "examples/lua/aogmaneo.lua"))
        subprocess.run(arguments, check=True, env={**os.environ, "ASAN_OPTIONS": "detect_leaks=1"})

    def test_python_marshaling_and_native_lifetime(self):
        self.run_bindings("python")

    def test_real_lua_interpreter_and_native_lifetime(self):
        self.run_bindings("lua")

    def test_cleanup_and_stop_guards_in_both_runtimes(self):
        for runtime, prefix in (("python", "python"), ("lua", "solua")):
            source = (ROOT / f"src/apps/solar_os_{runtime}.c").read_text()
            self.assertEqual(source.count(prefix + "_aogmaneo_cleanup();"), 3)
            self.assertIn(prefix + "_jet_active() || " + prefix + "_aogmaneo_active()", source)
            self.assertIn('#include "solar_os_' + runtime + '_aogmaneo.inc"', source)

    def test_python_example_parses(self):
        ast.parse((ROOT / "examples/python/aogmaneo.py").read_text())


if __name__ == "__main__":
    unittest.main()
