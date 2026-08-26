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


class ScriptOscBindingsTest(unittest.TestCase):
    def test_osc_service_has_python_lua_parity(self):
        methods = (
            "bindings",
            "bind_stream",
            "bind_event",
            "bind_control",
            "unbind",
            "clear",
            "encode_float",
            "encode_int",
            "dispatch",
            "limits",
        )
        for method in methods:
            self.assertIn(
                f"SOLAR_OS_SCRIPT_API_FUNCTION(osc, {method}, {method});",
                API_DESCRIPTOR,
            )
        self.assertIn("#if SOLAR_OS_PACKAGE_SERVICE_OSC", API_DESCRIPTOR)

    def test_binding_configuration_and_telemetry_fields_match(self):
        fields = (
            "id",
            "name",
            "source_type",
            "value_type",
            "source",
            "address",
            "interval_ms",
            "rate_hz",
            "delta",
            "send_always",
            "edge",
            "source_available",
            "has_value",
            "last_value",
            "has_sent_value",
            "last_sent_value",
            "last_sample_ms",
            "last_send_ms",
            "sent",
            "send_errors",
            "source_errors",
            "last_error",
            "last_error_name",
        )
        for field in fields:
            self.assertIn(f'"{field}"', PYTHON_SOURCE)
            self.assertIn(f'"{field}"', LUA_SOURCE)

    def test_codec_dispatch_and_limits_use_native_service(self):
        tokens = (
            "solar_os_osc_encode_float(",
            "solar_os_osc_encode_int(",
            "solar_os_osc_dispatch_packet(",
            "SOLAR_OS_OSC_PACKET_MAX",
            "SOLAR_OS_OSC_BINDING_MAX",
            "SOLAR_OS_OSC_BUNDLE_DEPTH_MAX",
            "SOLAR_OS_OSC_PACKET_UPDATE_MAX",
            "SOLAR_OS_OSC_RATE_MIN_MILLIHZ",
            "SOLAR_OS_OSC_RATE_MAX_MILLIHZ",
        )
        for token in tokens:
            self.assertIn(token, PYTHON_SOURCE)
            self.assertIn(token, LUA_SOURCE)

    def test_dispatch_returns_matching_counters(self):
        for field in (
            "messages", "applied", "unknown_paths", "rejected_values"
        ):
            self.assertIn(f'"{field}"', PYTHON_SOURCE)
            self.assertIn(f'"{field}"', LUA_SOURCE)


if __name__ == "__main__":
    unittest.main()
