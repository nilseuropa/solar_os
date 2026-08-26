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


class ScriptMidiBindingsTest(unittest.TestCase):
    def test_midi_service_has_python_lua_parity(self):
        methods = (
            ("status", "status"),
            ("send", "send"),
            ("note_on", "note_on"),
            ("note_off", "note_off"),
            ("cc", "cc"),
            ("program", "program"),
            ("read", "read"),
            ("receive", "read"),
            ("close", "close"),
            ("streams", "streams"),
            ("stream_add", "stream_add"),
            ("stream_remove", "stream_remove"),
            ("stream_clear", "stream_clear"),
        )
        for public_name, native_name in methods:
            self.assertIn(
                "SOLAR_OS_SCRIPT_API_FUNCTION("
                f"midi, {public_name}, {native_name});",
                API_DESCRIPTOR,
            )
        self.assertIn("#if SOLAR_OS_PACKAGE_SERVICE_MIDI", API_DESCRIPTOR)

    def test_receive_is_bounded_non_consuming_and_lifecycle_owned(self):
        for source, prefix in (
            (PYTHON_SOURCE, "python"),
            (LUA_SOURCE, "solua"),
        ):
            self.assertIn("solar_os_midi_subscribe(", source)
            self.assertIn("solar_os_midi_receive(", source)
            self.assertIn("solar_os_midi_unsubscribe(", source)
            self.assertIn(f"{prefix}_midi_destroy();", source)
            self.assertGreaterEqual(source.count(f"{prefix}_midi_destroy();"), 3)
            self.assertIn("MIDI_READ_MAX_MS 60000U", source)
            self.assertIn("vTaskDelay(pdMS_TO_TICKS(10))", source)

    def test_status_messages_and_streams_return_matching_fields(self):
        fields = (
            "running",
            "bus",
            "rx_bytes",
            "rx_messages",
            "tx_bytes",
            "tx_messages",
            "parser_unsupported",
            "subscriber_drops",
            "tx_drops",
            "last_error",
            "last_error_name",
            "cc_streams",
            "subscribed",
            "status",
            "length",
            "type",
            "channel",
            "data1",
            "data2",
            "controller",
            "has_value",
            "updates",
        )
        for field in fields:
            self.assertIn(f'"{field}"', PYTHON_SOURCE)
            self.assertIn(f'"{field}"', LUA_SOURCE)

    def test_raw_transmission_uses_native_validation(self):
        for source in (PYTHON_SOURCE, LUA_SOURCE):
            self.assertIn("solar_os_midi_message_length(status)", source)
            self.assertIn("solar_os_midi_send(message)", source)
            self.assertIn("expected MIDI channel 1..16", source)
            self.assertIn("expected MIDI data 0..127", source)


if __name__ == "__main__":
    unittest.main()
