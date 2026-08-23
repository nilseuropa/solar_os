import re
import unittest
from collections import defaultdict
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
PYTHON_SOURCE = (REPOSITORY / "src/apps/solar_os_python.c").read_text(
    encoding="utf-8"
)
LUA_SOURCE = (REPOSITORY / "src/apps/solar_os_lua.c").read_text(encoding="utf-8")
PYTHON_BINDINGS = PYTHON_SOURCE + (
    REPOSITORY / "src/apps/solar_os_python_dsp.inc"
).read_text(encoding="utf-8")
LUA_BINDINGS = LUA_SOURCE + (
    REPOSITORY / "src/apps/solar_os_lua_dsp.inc"
).read_text(encoding="utf-8")
DESCRIPTOR = (REPOSITORY / "src/apps/solar_os_script_api.inc").read_text(
    encoding="utf-8"
)
HID_KEYCODES = (REPOSITORY / "src/services/solar_os_hid_keycodes.inc").read_text(
    encoding="utf-8"
)


class ScriptBindingDescriptorTest(unittest.TestCase):
    def test_both_interpreters_expand_the_shared_descriptor_once(self):
        include = '#include "solar_os_script_api.inc"'
        self.assertEqual(PYTHON_SOURCE.count(include), 1)
        self.assertEqual(LUA_SOURCE.count(include), 1)
        self.assertNotIn("solar_os_script_bus_api.inc", PYTHON_SOURCE)
        self.assertNotIn("solar_os_script_bus_api.inc", LUA_SOURCE)

    def test_all_service_modules_are_descriptor_registered(self):
        modules = re.findall(
            r"^SOLAR_OS_SCRIPT_API_MODULE_BEGIN\((\w+)\);$",
            DESCRIPTOR,
            re.MULTILINE,
        )
        self.assertEqual(len(modules), 35)
        self.assertEqual(len(modules), len(set(modules)))
        self.assertNotRegex(PYTHON_SOURCE, r'python_new_submodule\(module,\s*"')
        self.assertNotRegex(LUA_SOURCE, r'solua_new_submodule\(L,\s*solaros,\s*"')

    def test_default_handlers_exist_in_both_interpreters(self):
        functions = re.findall(
            r"^SOLAR_OS_SCRIPT_API_FUNCTION\("
            r"(\w+),\s*(\w+),\s*(\w+)\);$",
            DESCRIPTOR,
            re.MULTILINE,
        )
        self.assertGreater(len(functions), 150)
        for module, _public_name, native_name in functions:
            python_handler = f"solaros_{module}_{native_name}_obj"
            lua_handler = f"solua_{module}_{native_name}"
            self.assertTrue(
                python_handler in PYTHON_BINDINGS
                or f"PYTHON_{module.upper()}_BODY_METHOD({native_name},"
                in PYTHON_BINDINGS,
                python_handler,
            )
            self.assertTrue(
                lua_handler in LUA_BINDINGS
                or f"SOLUA_{module.upper()}_BODY_METHOD({native_name},"
                in LUA_BINDINGS,
                lua_handler,
            )

    def test_named_and_nested_handlers_exist_in_both_interpreters(self):
        named = re.findall(
            r"SOLAR_OS_SCRIPT_API_FUNCTION_NAMED\(\s*"
            r"(\w+),\s*(\w+),\s*(\w+),\s*(\w+)\);",
            DESCRIPTOR,
        )
        self.assertEqual(len(named), 2)
        for _module, _public_name, python_native, lua_native in named:
            self.assertIn(python_native, PYTHON_BINDINGS)
            self.assertIn(lua_native, LUA_BINDINGS)

        nested = re.findall(
            r"^SOLAR_OS_SCRIPT_API_SUBMODULE_FUNCTION\("
            r"(\w+),\s*(\w+),\s*(\w+),\s*(\w+)\);$",
            DESCRIPTOR,
            re.MULTILINE,
        )
        self.assertEqual(len(nested), 9)
        for module, submodule, _public_name, native_name in nested:
            self.assertIn(
                f"solaros_{module}_{submodule}_{native_name}_obj",
                PYTHON_BINDINGS,
            )
            self.assertIn(
                f"solua_{module}_{submodule}_{native_name}",
                LUA_BINDINGS,
            )

    def test_each_module_has_unique_public_names(self):
        entries = defaultdict(list)
        for macro in ("INT", "UINT", "FUNCTION"):
            for module, public_name in re.findall(
                rf"^SOLAR_OS_SCRIPT_API_{macro}\("
                rf"(\w+),\s*(\w+),[^\n]*\);$",
                DESCRIPTOR,
                re.MULTILINE,
            ):
                entries[module].append(public_name)
        for module, public_name in re.findall(
            r"SOLAR_OS_SCRIPT_API_FUNCTION_NAMED\(\s*"
            r"(\w+),\s*(\w+),",
            DESCRIPTOR,
        ):
            entries[module].append(public_name)

        for module, public_names in entries.items():
            self.assertEqual(
                len(public_names),
                len(set(public_names)),
                f"duplicate public binding in solaros.{module}",
            )

        nested_count = len(
            re.findall(
                r"^SOLAR_OS_SCRIPT_API_SUBMODULE_FUNCTION\(",
                DESCRIPTOR,
                re.MULTILINE,
            )
        )
        hid_keycode_count = len(
            re.findall(
                r"^SOLAR_OS_HID_KEY_CONSTANT\(", HID_KEYCODES, re.MULTILINE
            )
        )
        self.assertEqual(
            sum(map(len, entries.values())) + nested_count + hid_keycode_count,
            455,
        )

    def test_shared_tui_helpers_have_python_lua_parity(self):
        for name in (
            "layout", "cell", "title", "help", "tab", "list_move",
            "input_edit", "input",
        ):
            entry = f"SOLAR_OS_SCRIPT_API_FUNCTION(tui, {name}, {name});"
            self.assertIn(entry, DESCRIPTOR)
            self.assertIn(f"solaros_tui_{name}_obj", PYTHON_BINDINGS)
            self.assertIn(f"solua_tui_{name}", LUA_BINDINGS)

    def test_tui_input_mask_has_python_lua_parity(self):
        self.assertIn(
            "MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(solaros_tui_input_obj, 7, 9",
            PYTHON_SOURCE,
        )
        self.assertIn("n_args > 8", PYTHON_SOURCE)
        self.assertIn("lua_isnoneornil(L, 9)", LUA_SOURCE)
        for source in (PYTHON_SOURCE, LUA_SOURCE):
            self.assertIn("solar_os_tui_draw_input_ex", source)


if __name__ == "__main__":
    unittest.main()
