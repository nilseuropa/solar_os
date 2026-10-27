from pathlib import Path
import re
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class AudioToneTest(unittest.TestCase):
    def test_short_tones_and_alarm_sequence_on_buffered_output(self):
        source = (ROOT / "src/services/solar_os_audio.c").read_text()
        constants = "\n".join(re.findall(
            r"^#define (?:AUDIO_FRAME_CHUNK|AUDIO_TONE_[A-Z_]+) .+$",
            source, re.M))
        functions = production_functions(source, (
            "audio_volume_arg_valid", "audio_tone_write_segment",
            "audio_play_tone_sequence_locked", "audio_play_tone_locked"))
        with tempfile.TemporaryDirectory(prefix="solaros-audio-tones-") as tmp:
            Path(tmp, "audio_tones.inc").write_text(constants + "\n" + functions)
            binary = Path(tmp, "tones")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src/services"),
                str(ROOT / "tests/host/audio_tones_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)

    def test_worker_keeps_one_stream_for_the_sequence(self):
        source = (ROOT / "src/services/solar_os_audio.c").read_text()
        worker = production_functions(source, ("audio_tone_worker",))
        self.assertIn("audio_play_tone_sequence_locked(", worker)
        self.assertNotIn("audio_play_tone_locked(", worker)


if __name__ == "__main__":
    unittest.main()
