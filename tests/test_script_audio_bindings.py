import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
PYTHON_SOURCE = (REPOSITORY / "src/apps/solar_os_python.c").read_text(
    encoding="utf-8"
)
LUA_SOURCE = (REPOSITORY / "src/apps/solar_os_lua.c").read_text(encoding="utf-8")
AUDIO_HEADER = (REPOSITORY / "src/services/solar_os_audio.h").read_text(
    encoding="utf-8"
)
AUDIO_SOURCE = (REPOSITORY / "src/services/solar_os_audio.c").read_text(
    encoding="utf-8"
)
API_DESCRIPTOR = (REPOSITORY / "src/apps/solar_os_script_api.inc").read_text(
    encoding="utf-8"
)


class ScriptAudioBindingsTest(unittest.TestCase):
    def test_capture_is_shared_and_bounded(self):
        self.assertIn(
            "SOLAR_OS_SCRIPT_API_FUNCTION(audio, capture, capture);",
            API_DESCRIPTOR,
        )
        self.assertIn("SOLAR_OS_AUDIO_CAPTURE_MAX_FRAMES 4096U", AUDIO_HEADER)
        self.assertIn("SOLAR_OS_AUDIO_CAPTURE_MAX_CHANNELS 2U", AUDIO_HEADER)

    def test_capture_uses_the_default_typed_input_and_always_closes_it(self):
        capture = AUDIO_SOURCE.split("esp_err_t solar_os_audio_capture(", 1)[1].split(
            "static bool audio_stream_format_requested", 1
        )[0]
        self.assertIn("solar_os_audio_open_default(", capture)
        self.assertIn("SOLAR_OS_STREAM_DIRECTION_SOURCE", capture)
        self.assertIn("SOLAR_OS_STREAM_AUDIO_S16_LE", capture)
        self.assertIn("solar_os_stream_read(&stream", capture)
        self.assertIn("captured_format.frames_per_block", capture)
        self.assertIn("block_frames > remaining_frames", capture)
        self.assertIn("const size_t read_bytes = block_frames * frame_bytes", capture)
        self.assertGreaterEqual(capture.count("solar_os_stream_close(&stream);"), 3)

    def test_python_and_lua_return_the_same_format_fields(self):
        fields = (
            "sample_format",
            "sample_rate",
            "channels",
            "bits_per_sample",
        )
        for field in fields:
            self.assertIn(f'"{field}"', PYTHON_SOURCE)
            self.assertIn(f'"{field}"', LUA_SOURCE)
        self.assertIn("mp_obj_new_tuple(2, result)", PYTHON_SOURCE)
        self.assertIn("return 2;", LUA_SOURCE)


if __name__ == "__main__":
    unittest.main()
