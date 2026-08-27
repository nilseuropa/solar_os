import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
PYTHON_SOURCE = (REPOSITORY / "src/apps/solar_os_python.c").read_text(
    encoding="utf-8"
)
LUA_SOURCE = (REPOSITORY / "src/apps/solar_os_lua.c").read_text(encoding="utf-8")
DESCRIPTOR = (REPOSITORY / "src/apps/solar_os_script_api.inc").read_text(
    encoding="utf-8"
)


class ScriptInputBindingsTest(unittest.TestCase):
    def test_input_module_has_mirrored_bounded_read_api(self):
        for function in ("sources", "read", "clear", "status"):
            self.assertIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(input, {function}, {function});",
                DESCRIPTOR,
            )
            self.assertIn(f"solaros_input_{function}_obj", PYTHON_SOURCE)
            self.assertIn(f"solua_input_{function}", LUA_SOURCE)

        self.assertIn("PYTHON_DEVICE_INPUT_QUEUE_LEN 16", PYTHON_SOURCE)
        self.assertIn("SOLUA_DEVICE_INPUT_QUEUE_LEN 16U", LUA_SOURCE)
        self.assertIn("input read limited to 60000 ms", PYTHON_SOURCE)
        self.assertIn("input read limited to 60000 ms", LUA_SOURCE)

    def test_runtimes_opt_in_and_forward_pointer_and_axis_events(self):
        flags = (
            ".flags = SOLAR_OS_APP_FLAG_POINTER_EVENTS | "
            "SOLAR_OS_APP_FLAG_AXIS_EVENTS"
        )
        for source in (PYTHON_SOURCE, LUA_SOURCE):
            self.assertIn(flags, source)
            self.assertIn("event->type == SOLAR_OS_EVENT_POINTER", source)
            self.assertIn("event->type == SOLAR_OS_EVENT_AXIS", source)
            self.assertIn("sizeof(solar_os_event_t)", source)
            self.assertIn("device_input_dropped", source)

    def test_pointer_and_axis_fields_match_between_runtimes(self):
        fields = (
            "type",
            "source",
            "source_name",
            "source_class",
            "source_class_name",
            "pointer_id",
            "mode",
            "mode_name",
            "action",
            "action_name",
            "x",
            "y",
            "delta_x",
            "delta_y",
            "buttons",
            "target",
            "axis",
            "axis_name",
            "value",
            "delta",
        )
        for field in fields:
            self.assertIn(f'"{field}"', PYTHON_SOURCE)
            self.assertIn(f'"{field}"', LUA_SOURCE)

    def test_input_constants_cover_discovery_pointer_and_axes(self):
        constants = (
            "SOURCE_TOUCH",
            "SOURCE_MOUSE",
            "CAP_POINTER_ABSOLUTE",
            "CAP_POINTER_RELATIVE",
            "CAP_AXIS_EVENTS",
            "MODE_ABSOLUTE",
            "MODE_RELATIVE",
            "ACTION_MOVE",
            "ACTION_PRESS",
            "ACTION_RELEASE",
            "BUTTON_PRIMARY",
            "BUTTON_SECONDARY",
            "BUTTON_MIDDLE",
            "AXIS_X",
            "AXIS_Y",
            "AXIS_Z",
            "AXIS_RX",
            "AXIS_RY",
            "AXIS_RZ",
        )
        for constant in constants:
            self.assertRegex(
                DESCRIPTOR,
                rf"SOLAR_OS_SCRIPT_API_(?:INT|UINT)\(input, {constant},",
            )


if __name__ == "__main__":
    unittest.main()
