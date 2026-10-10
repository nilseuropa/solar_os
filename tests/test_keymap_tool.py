import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import runpy
import shlex
import shutil
import subprocess
import sys
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))
import generate_keymaps
import keymap
from solaros_keymap import KeymapError, PROFILE_IDS, load_profile, merge, read_json
from solaros_keymap_build import dependencies, resolve_selection


class _KeymapFixture:
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.profile = load_profile("inputronic-keyboard")

    def write(self, name, document):
        path = self.directory / name
        path.write_text(json.dumps(document), encoding="utf-8")
        return path

    def cli(self, *args, answers=None):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()), \
                mock.patch("builtins.input", side_effect=answers), \
                mock.patch.object(sys.stdin, "isatty", return_value=False):
            return keymap.main(list(args))


class KeymapToolTest(_KeymapFixture, unittest.TestCase):
    def test_profile_legends_and_defaults(self):
        expected = {"FN1": 78, "FN2": 79, "FN3": 17, "FN4": 18, "FN5": 19, "FN6": 20}
        for name, physical in expected.items():
            self.assertEqual(self.profile.select(name.lower()), physical)
            self.assertEqual(self.profile.layers[0][physical], {"raw": True})
        self.assertEqual(self.profile.select("A"), 52)
        self.assertEqual(self.profile.layers[0][52], {"usage": 4})
        pager = load_profile("lilygo-pager-keyboard")
        self.assertEqual(pager.layers[0][31], {"usage": 44, "layer_tap": True})
        self.assertEqual(pager.layers[1][26], {"key": 33, "alt_block": True})

    def test_partial_maps_replace_entries_and_preserve_others(self):
        result = merge(self.profile, {"schema": 1, "keys": [{"physical": 78, "usage": 58}]})
        self.assertEqual(result[0][78], {"usage": 58})  # Original raw flag is replaced.
        self.assertEqual(result[0][52], self.profile.layers[0][52])
        self.assertEqual(result[0][79], {"raw": True})
        self.assertEqual(self.profile.layers[0][78], {"raw": True})
        row = merge(self.profile, {"schema": 1, "keys": [{"row": 7, "col": 7, "usage": 58}]})
        self.assertEqual(result, row)

    def test_generic_geometry_keeps_controller_stride_and_rejects_inactive_cells(self):
        profile = load_profile("tca8418", rows=3, cols=3)
        self.assertEqual(set(profile.names), {1, 2, 3, 11, 12, 13, 21, 22, 23})
        self.assertEqual(profile.select("A"), 11)
        self.assertEqual(merge(profile, {"schema": 1, "keys": [{"row": 1, "col": 0, "usage": 5}]})[0][11], {"usage": 5})
        with self.assertRaises(KeymapError):
            merge(profile, {"schema": 1, "keys": [{"physical": 4}]})
        with self.assertRaises(KeymapError):
            load_profile("inputronic-keyboard", rows=3)

    def test_cli_create_and_edit_keep_all_explicit_assignments(self):
        path = self.directory / "map.json"
        self.assertEqual(self.cli("create", "--profile", self.profile.id, "--set", "FN1=F1",
                                 "--set", "FN2=raw", "--output", str(path)), 0)
        self.assertEqual(read_json(path)["keys"], [{"physical": 78, "usage": 58},
                                                  {"physical": 79, "raw": True}])
        self.assertEqual(self.cli("edit", "--profile", self.profile.id, "--input", str(path),
                                 "--set", "FN3=Escape"), 0)
        result = merge(self.profile, read_json(path))
        self.assertEqual(result[0][78], {"usage": 58})
        self.assertEqual(result[0][17], {"usage": 41})
        self.assertIn({"physical": 79, "raw": True}, read_json(path)["keys"])

    def test_failed_edit_and_exclusive_create_preserve_existing_file(self):
        path = self.write("map.json", {"schema": 1, "keys": []})
        original = path.read_bytes()
        self.assertEqual(self.cli("create", "--profile", self.profile.id, "--set", "FN1=F1",
                                 "--output", str(path)), 1)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(self.cli("edit", "--profile", self.profile.id, "--input", str(path),
                                 "--set", "FN1=tap:Ctrl"), 1)
        self.assertEqual(path.read_bytes(), original)
        self.assertEqual(list(self.directory.glob(".map.json.*")), [])

    def test_interactive_fn_assignments_and_symbols(self):
        path = self.directory / "map.json"
        # Printed FN names determine prompt order rather than matrix wiring.
        self.assertEqual(self.cli("create", "--profile", self.profile.id, "--output", str(path),
                                 "--interactive", answers=["F1", "Escape", "", "", "", ""]), 0)
        self.assertEqual(read_json(path)["keys"], [{"physical": 78, "usage": 58},
                                                  {"physical": 79, "usage": 41}])
        self.assertEqual(self.cli("edit", "--profile", self.profile.id, "--input", str(path),
                                 "--set", "FN3=tap:Space", "--symbol", "A=char:!"), 0)
        result = merge(self.profile, read_json(path))
        self.assertEqual(result[0][17], {"usage": 44, "layer_tap": True})
        self.assertEqual(result[1][52], {"key": 33})

    def test_show_validate_profiles_and_unknown_assignments(self):
        path = self.write("map.json", {"schema": 1, "keys": []})
        self.assertEqual(self.cli("profiles"), 0)
        self.assertEqual(self.cli("show", "--profile", self.profile.id), 0)
        self.assertEqual(self.cli("validate", "--profile", self.profile.id, "--input", str(path)), 0)
        for value in ("missing=F1", "FN1=no-such-action", "FN1=char:ab", "FN1=raw", "FN1=usage:1"):
            expected = 0 if value == "FN1=raw" else 1
            self.assertEqual(self.cli("create", "--profile", self.profile.id, "--set", value,
                                     "--output", str(self.directory / "new.json")), expected)

    def test_invalid_maps_and_limits(self):
        invalid = [
            {}, {"schema": 2, "keys": []}, {"schema": True, "keys": []},
            {"schema": 1, "keys": None}, {"schema": 1, "extra": 1, "keys": []},
            {"schema": 1, "keys": [{"physical": 78, "raw": True, "usage": 58}]},
            {"schema": 1, "keys": [{"physical": 78, "usage": 1}]},
            {"schema": 1, "keys": [{"physical": 78.5, "usage": 58}]},
            {"schema": 1, "keys": [{"physical": 78, "row": 7, "col": 7}]},
            {"schema": 1, "keys": [{"physical": 0}]},
            {"schema": 1, "keys": [{"physical": 81}]},
            {"schema": 1, "keys": [{"physical": 78, "key": 256}]},
            {"schema": 1, "keys": [{"physical": 78, "layer_tap": "yes"}]},
            {"schema": 1, "keys": [{"physical": 78, "layer_tap": True},
                                     {"physical": 79, "layer_tap": True}]},
            {"schema": 1, "keys": [{"physical": 78}, {"row": 7, "col": 7}]},
            {"schema": 1, "symbols": [{"physical": 78, "layer_tap": True}]},
        ]
        for document in invalid:
            with self.subTest(document=document), self.assertRaises(KeymapError):
                merge(self.profile, document)
        for text in ('{"schema":1,"keys":[],"schema":1}', '{"schema":NaN,"keys":[]}',
                     '[[[[[[]]]]]]', '{} trailing', '{}\0', ' ' * 16385):
            path = self.directory / "bad.json"
            path.write_text(text)
            with self.subTest(text=text[:80]), self.assertRaises(ValueError):
                read_json(path)

    def test_selection_index_paths_and_invalid_combinations(self):
        first = self.write("first map.json", {"schema": 1, "keys": []})
        second = self.write("pager.json", {"schema": 1, "keys": []})
        index = self.write("index.json", {"schema": 1, "profiles": {
            self.profile.id: first.name, "lilygo-pager-keyboard": second.name}})
        self.assertEqual(resolve_selection(self.directory, index=index.name), {
            self.profile.id: first, "lilygo-pager-keyboard": second})
        self.assertEqual(set(dependencies(self.directory, index=index.name)), {index, first, second})
        for profile, file, manifest in ((self.profile.id, "", ""), ("", first.name, ""),
            ("unknown", first.name, ""), (self.profile.id, first.name, index.name),
            (self.profile.id, "missing.json", "")):
            with self.assertRaises(KeymapError):
                resolve_selection(self.directory, profile, file, manifest)

    def test_generator_tracks_overrides_and_removal(self):
        output = self.directory / "generated"
        first = self.write("custom.json", {"schema": 1, "keys": [{"physical": 78, "usage": 58}]})
        generate_keymaps.generate(output, root=self.directory)
        standard = {path.name: path.read_bytes() for path in output.glob("*.h")}
        generate_keymaps.generate(output, root=self.directory, profile=self.profile.id, file=first.name)
        custom = (output / "solar_os_inputronic_keymap_generated.h").read_text()
        self.assertIn("[78] = {58, 0, 0, 0}", custom)
        self.assertEqual((output / "solar_os_pager_keymap_generated.h").read_bytes(),
                         standard["solar_os_pager_keymap_generated.h"])
        bad = self.write("bad.json", {"schema": 99, "keys": []})
        with self.assertRaises(KeymapError):
            generate_keymaps.generate(output, root=self.directory, profile=self.profile.id, file=bad.name)
        self.assertEqual((output / "solar_os_inputronic_keymap_generated.h").read_text(), custom)
        generate_keymaps.generate(output, root=self.directory)
        self.assertEqual({path.name: path.read_bytes() for path in output.glob("*.h")}, standard)


class PlatformIOKeymapTest(_KeymapFixture, unittest.TestCase):
    def run_selection(self, variables, custom=None, cmake=""):
        build = self.directory / "build"
        build.mkdir(exist_ok=True)
        class Board(dict):
            def update(self, name, value):
                self[name] = value
        board = Board({"build.cmake_extra_args": cmake})
        class Environment:
            def subst(self, value):
                return {"$PROJECT_DIR": str(ROOT), "$BUILD_DIR": str(build)}[value]
            def __getitem__(self, name):
                return "waveshare_esp32_s3_sim7670g_4g"
            def BoardConfig(self):
                return board
            def GetProjectOption(self, name, default=""):
                return (custom or {}).get(name, default)
        selection = {name: "" for name in os.environ if name.startswith("SOLAR_OS_")}
        selection.update(variables)
        with mock.patch.dict(os.environ, selection), \
             mock.patch("solaros_build_lock.acquire_project_build_lock"), \
             contextlib.redirect_stdout(io.StringIO()):
            runpy.run_path(str(ROOT / "scripts/platformio_solaros_flavor.py"),
                           init_globals={"env": Environment(), "Import": lambda _: None})
        return build, shlex.split(board["build.cmake_extra_args"])

    def test_pio_reconfigures_when_file_changes_or_override_is_removed(self):
        path = self.write("map with spaces.json", {"schema": 1, "keys": [{"physical": 78, "usage": 58}]})
        options = {"SOLAR_OS_KEYMAP_PROFILE": self.profile.id, "SOLAR_OS_KEYMAP_FILE": str(path)}
        build, flags = self.run_selection(options)
        self.assertIn(f"-DSOLAR_OS_KEYMAP_FILE={path}", flags)
        cache = build / "CMakeCache.txt"
        cache.write_text("test cache")
        self.run_selection(options)
        self.assertTrue(cache.exists())
        path.write_text('{"schema":1,"keys":[{"physical":78,"usage":59}]}')
        self.run_selection(options)
        self.assertFalse(cache.exists())
        cache.write_text("test cache")
        _, flags = self.run_selection({})
        self.assertFalse(cache.exists())
        self.assertIn("-DSOLAR_OS_KEYMAP_FILE=", flags)

    def test_pio_custom_options_and_explicit_cmake_flags(self):
        path = self.write("map.json", {"schema": 1, "keys": []})
        _, flags = self.run_selection({}, custom={"custom_solaros_keymap_profile": self.profile.id,
                                                  "custom_solaros_keymap_file": str(path)})
        self.assertIn(f"-DSOLAR_OS_KEYMAP_FILE={path}", flags)
        _, flags = self.run_selection({}, cmake=f"-DSOLAR_OS_KEYMAP_PROFILE={self.profile.id} "
                                               f"-DSOLAR_OS_KEYMAP_FILE={path}")
        self.assertIn(f"-DSOLAR_OS_KEYMAP_FILE={path}", flags)


class NativeKeymapParityTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.directory = Path(cls.temporary.name)
        cls.cjson = Path(os.environ.get("CJSON_DIR", str(Path.home() / ".platformio/packages/framework-espidf/components/json/cJSON")))
        if not (cls.cjson / "cJSON.c").exists() or not shutil.which(os.environ.get("CC", "cc")):
            raise unittest.SkipTest("native parity checks require a C compiler and ESP-IDF cJSON")
        cls.standard = cls.compile("standard")

    @classmethod
    def compile(cls, name, **selection):
        directory = cls.directory / name
        generate_keymaps.generate(directory, root=cls.directory, **selection)
        host = ROOT / "tests/host"
        sources = [host / "keymap_profiles_dump.c", host / "keymap_memory_stub.c"]
        sources += [ROOT / "src/services" / (name + ".c") for name in (
            "solar_os_input_keymap_mapper", "solar_os_input_keymap_json", "solar_os_matrix_keyboard",
            "solar_os_inputronic_keyboard_profile", "solar_os_lilygo_pager_keyboard")]
        command = [os.environ.get("CC", "cc"), "-std=c11", "-Wall", "-Wextra", "-Werror",
                   "-ffunction-sections", "-fdata-sections", "-include", str(host / "compat.h")]
        for include in (directory, host / "inputronic_stubs", host / "expansion_stubs", host,
                        ROOT / "src", ROOT / "src/services", ROOT / "include", cls.cjson):
            command += ["-I", str(include)]
        executable = directory / "dump"
        command += list(map(str, sources)) + [str(cls.cjson / "cJSON.c"), "-Wl,--gc-sections", "-lm", "-o", str(executable)]
        subprocess.run(command, check=True, capture_output=True, text=True)
        return executable

    def dump(self, executable, profile, path=None):
        result = subprocess.run([str(executable), profile] + ([str(path)] if path else []),
                                capture_output=True, text=True)
        return result.returncode, json.loads(result.stdout) if result.returncode == 0 else None

    def test_all_generated_entries_match_shared_profiles(self):
        for name in PROFILE_IDS:
            profile = load_profile(name)
            code, result = self.dump(self.standard, name)
            self.assertEqual(code, 0)
            self.assertEqual((result["rows"], result["cols"]), (profile.rows, profile.cols))
            for layer, physical, usage, key, shift, flags in result["keys"]:
                entry = profile.layers[layer].get(physical, {})
                self.assertEqual([usage, key, shift, flags], [entry.get("usage", 0), entry.get("key", 0),
                    entry.get("shift_key", 0), 1 if entry.get("raw") else 2 if entry.get("layer_tap") else 4 if entry.get("alt_block") else 0])

    def test_standard_profiles_preserve_previous_firmware_mappings(self):
        # Golden dumps from the C factories before moving their tables to JSON.
        # Include every physical position in both layers, not only text keys.
        expected = {
            "tca8418": "57cd250de89f0d7157a73a978c801d64e88728ad440de4288defd14558870d01",
            "inputronic-keyboard": "979f44eb7e1b73541442f130cf4d596d0762d81b3661e87fd2eb9836a9c5a5b0",
            "lilygo-pager-keyboard": "e4a1c56ca9963418a1d64a79fb7d894e6ba115afa453e47cc0776c8be284ebe4",
        }
        for profile, digest in expected.items():
            dump = subprocess.check_output([str(self.standard), profile])
            self.assertEqual(hashlib.sha256(dump).hexdigest(), digest, profile)

    def test_custom_compiled_defaults_remain_base_for_runtime_sparse_loads(self):
        path = self.directory / "custom.json"
        path.write_text('{"schema":1,"keys":[{"physical":78,"usage":58},{"physical":79,"usage":41}]}')
        executable = self.compile("custom", profile="inputronic-keyboard", file=str(path))
        code, result = self.dump(executable, "inputronic-keyboard")
        self.assertEqual(code, 0)
        self.assertIn([0, 78, 58, 0, 0, 0], result["keys"])
        self.assertIn([0, 52, 4, 0, 0, 0], result["keys"])
        path.write_text('{"schema":1,"keys":[{"physical":79,"raw":true}]}')
        code, result = self.dump(executable, "inputronic-keyboard", path)
        self.assertEqual(code, 0)
        self.assertIn([0, 78, 58, 0, 0, 0], result["keys"])
        self.assertIn([0, 79, 0, 0, 0, 1], result["keys"])

    def test_firmware_and_desktop_accept_same_json_corpus(self):
        corpus = [
            '{"schema":1,"keys":[]}',
            '{"schema":1.0,"keys":[{"physical":78.0,"usage":58.0}]}',
            '{"schema":1,"keys":[{"row":7,"col":7,"usage":58}]}',
            '{"schema":1,"keys":[{"physical":78,"raw":true}]}',
            '{"schema":1,"keys":[{"physical":78}]}',
            '{"schema":1,"keys":[{"physical":78,"layer_tap":true}],"symbols":[{"physical":52,"key":33}]}',
            '{}', '{"schema":true,"keys":[]}', '{"schema":1,"keys":null}',
            '{"schema":1,"keys":[],"schema":1}', '{"schema":1,"keys":[]} trailing',
            '{"schema":1,"keys":[{"physical":78,"usage":1}]}',
            '{"schema":1,"keys":[{"physical":78,"usage":1e300}]}',
            '{"schema":1,"keys":[{"physical":78,"raw":true,"key":33}]}',
            '{"schema":1,"keys":[{"physical":78,"layer_tap":true,"usage":224}]}',
            '{"schema":1,"keys":[{"physical":78},{"row":7,"col":7}]}',
            '{"schema":1,"keys":[{"physical":78,"col":7}]}',
            '{"schema":1,"keys":[{"physical":0}]}',
            '{"schema":1,"keys":[{"physical":78,"extra":1}]}',
            '{"schema":1,"symbols":[{"physical":78,"layer_tap":true}]}',
            '{"schema":1,"keys":[{"physical":78,"layer_tap":true},{"physical":79,"layer_tap":true}]}',
        ]
        profile = load_profile("inputronic-keyboard")
        for text in corpus:
            path = self.directory / "corpus.json"
            path.write_text(text)
            try:
                merge(profile, read_json(path))
                accepted = True
            except ValueError:
                accepted = False
            code, _ = self.dump(self.standard, profile.id, path)
            self.assertEqual(code == 0, accepted, text)


if __name__ == "__main__":
    unittest.main()
