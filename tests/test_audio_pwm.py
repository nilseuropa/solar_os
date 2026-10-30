from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class AudioPwmTest(unittest.TestCase):
    def test_block_pacing_preserves_samples_without_underruns(self):
        source = (ROOT / "src/drivers/audio_pwm.c").read_text()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "audio_pwm_write.inc").write_text(production_functions(
                source, ("audio_pwm_write_s16",)))
            for tick_hz in (1000, 100):
                binary = Path(tmp, f"audio-pwm-{tick_hz}")
                subprocess.run([
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", f"-DTICK_HZ={tick_hz}",
                    f"-I{tmp}", str(ROOT / "tests/host/audio_pwm_pacing_test.c"),
                    "-o", str(binary),
                ], check=True)
                subprocess.run([str(binary)], check=True)
