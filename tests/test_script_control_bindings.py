import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
PYTHON_SOURCE = (REPOSITORY / "src/apps/solar_os_python.c").read_text(
    encoding="utf-8"
)
LUA_SOURCE = (REPOSITORY / "src/apps/solar_os_lua.c").read_text(
    encoding="utf-8"
)
API_DESCRIPTOR = (REPOSITORY / "src/apps/solar_os_script_api.inc").read_text(
    encoding="utf-8"
)


class ScriptControlBindingsTest(unittest.TestCase):
    def test_controls_and_parameters_have_python_lua_parity(self):
        methods = (
            "list",
            "get",
            "set",
            "create",
            "delete",
            "clear",
            "bindings",
            "bind_parameter",
            "bind_midi",
            "unbind",
        )
        for method in methods:
            self.assertIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(controls, {method}, {method});",
                API_DESCRIPTOR,
            )
        for method in ("list", "get", "set"):
            self.assertIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(parameters, {method}, {method});",
                API_DESCRIPTOR,
            )

    def test_control_diagnostics_and_binding_fields_match(self):
        fields = (
            "source_value",
            "generation",
            "samples",
            "updates",
            "read_errors",
            "last_error",
            "last_error_name",
            "target",
            "parameter",
            "midi_channel",
            "midi_controller",
            "pickup_latched",
            "last_target_value",
            "applied",
            "errors",
        )
        for field in fields:
            self.assertIn(f'"{field}"', PYTHON_SOURCE)
            self.assertIn(f'"{field}"', LUA_SOURCE)

    def test_parameter_metadata_and_authoritative_set_match(self):
        fields = (
            "path",
            "owner",
            "label",
            "unit",
            "minimum",
            "maximum",
            "step",
            "curve",
            "readable",
            "value",
            "error",
            "error_name",
        )
        for field in fields:
            self.assertIn(f'"{field}"', PYTHON_SOURCE)
            self.assertIn(f'"{field}"', LUA_SOURCE)
        for source in (PYTHON_SOURCE, LUA_SOURCE):
            self.assertIn("solar_os_parameter_set(path, value)", source)
            self.assertIn("solar_os_parameter_get(path, &value)", source)


if __name__ == "__main__":
    unittest.main()
