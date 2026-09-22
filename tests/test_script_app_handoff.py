import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
DESCRIPTOR = (ROOT / "src/apps/solar_os_script_api.inc").read_text(encoding="utf-8")
HEADER = (ROOT / "src/apps/solar_os_app_registry.h").read_text(encoding="utf-8")
REGISTRY = (ROOT / "src/apps/solar_os_app_registry.c").read_text(encoding="utf-8")
PYTHON = (ROOT / "src/apps/solar_os_python.c").read_text(encoding="utf-8")
LUA = (ROOT / "src/apps/solar_os_lua.c").read_text(encoding="utf-8")


class ScriptAppHandoffTest(unittest.TestCase):
    def test_handoff_methods_have_python_lua_parity(self):
        for method in ("launch", "open", "can_open"):
            self.assertIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(apps, {method}, {method});",
                DESCRIPTOR,
            )
            self.assertIn(f"solaros_apps_{method}_obj", PYTHON)
            self.assertIn(f"solua_apps_{method}", LUA)

    def test_registry_owns_discovery_and_handoff(self):
        for symbol in (
            "solar_os_app_discovery_count",
            "solar_os_app_discovery_get",
            "solar_os_app_registry_request_launch",
            "solar_os_app_registry_can_open",
            "solar_os_app_registry_request_open",
        ):
            self.assertIn(symbol, HEADER)
            self.assertIn(symbol, REGISTRY)
        self.assertIn('"playground:%s"', REGISTRY)
        self.assertIn("solar_os_playground_find_installed_app", REGISTRY)
        self.assertIn('solar_os_app_registry_find("web")', REGISTRY)
        self.assertIn("solar_os_app_registry_find_opener(resolved)", REGISTRY)

    def test_successful_handoff_terminates_interpreter_without_finishing_context(self):
        self.assertIn("python_apps_handoff", PYTHON)
        self.assertIn("mp_raise_type(&mp_type_SystemExit)", PYTHON)
        self.assertIn("solua_apps_handoff", LUA)
        self.assertIn("luaL_error(L, SOLUA_EXIT_MARKER)", LUA)
        for source in (PYTHON, LUA):
            self.assertIn("if (ctx->requested_app != NULL)", source)

    def test_list_defaults_to_installed_playground_apps(self):
        self.assertIn("solar_os_app_discovery_count(include_playground)", PYTHON)
        self.assertIn("solar_os_app_discovery_count(include_playground)", LUA)
        self.assertIn('python_kw_value(kw_args, "include_playground")', PYTHON)
        self.assertIn("include_obj == MP_OBJ_NULL", PYTHON)
        self.assertIn("MP_DEFINE_CONST_FUN_OBJ_KW(solaros_apps_list_obj", PYTHON)
        self.assertIn("lua_isnoneornil(L, 1) || lua_toboolean(L, 1)", LUA)


if __name__ == "__main__":
    unittest.main()
