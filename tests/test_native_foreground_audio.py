from pathlib import Path
import subprocess
import tempfile
import unittest
from test_foreground_presenter import production_functions
ROOT = Path(__file__).resolve().parents[1]
class ForegroundAudioTest(unittest.TestCase):
    def test_callback_join_and_task_ownership(self):
        source = (ROOT / "src/services/solar_os_native_foreground.c").read_text()
        names = ("foreground_current", "foreground_caller", "foreground_result",
                 "foreground_rgb565", "foreground_refresh_index2_palette",
                 "foreground_synth_render", "foreground_synth_close_state",
                 "foreground_synth_close", "foreground_synth_lock", "foreground_synth_unlock",
                 "foreground_synth_open", "foreground_audio_close", "foreground_audio_open",
                 "foreground_audio_write", "foreground_worker_cleanup")
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "audio.inc").write_text(production_functions(source, names))
            binary = Path(tmp, "audio")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", tmp, "-I", str(ROOT / "include"), "-I", str(ROOT / "tests/host"),
                            "-I", str(ROOT / "src/services"),
                            str(ROOT / "tests/host/native_foreground_audio_test.c"), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
