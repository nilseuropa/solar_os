from pathlib import Path
import subprocess
import tempfile
import unittest
import re
from tests.test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]

def native_key_pulse_includes(directory):
    source = (ROOT / "src/services/solar_os_native_foreground.c").read_text()
    constants = '\n'.join(re.findall(r'^#define FOREGROUND_KEY_PULSE_.*', source, re.M))
    typedef = re.search(r'typedef struct \{\s*solar_os_native_foreground_key_v1_t release;.*?\} foreground_key_pulse_t;', source, re.S).group()
    Path(directory, "native_key_pulse_type.inc").write_text(constants + '\n' + typedef)
    Path(directory, "native_key_pulse_read.inc").write_text(production_functions(source, ("foreground_read_key",)))

class NativeForegroundTest(unittest.TestCase):
    def test_logical_taps_and_physical_key_streams(self):
        with tempfile.TemporaryDirectory() as tmp:
            native_key_pulse_includes(tmp)
            binary = Path(tmp, "keys")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", tmp, "-I", str(ROOT / "include"),
                            "-I", str(ROOT / "tests/host"),
                            str(ROOT / "tests/host/native_foreground_keys_test.c"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_logical_arrows_through_doom_input_tics(self):
        module = ROOT / "modules/doom/main"
        with tempfile.TemporaryDirectory() as tmp:
            native_key_pulse_includes(tmp)
            defines = (module / "vendor/prboom/include/doomdef.h").read_text()
            Path(tmp, "doom_keydefs.inc").write_text('\n'.join(
                re.findall(r'^#define KEYD_.*', defines, re.M)))
            Path(tmp, "doom_input.inc").write_text(production_functions(
                (module / "video.c").read_text(),
                ("post", "solar_os_doom_reset_keys", "translate", "I_StartTic")))
            binary = Path(tmp, "doom-keys")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-Wno-sign-compare", "-DTEST_DOOM_INPUT",
                            "-I", tmp, "-I", str(ROOT / "include"),
                            "-I", str(ROOT / "tests/host"),
                            "-I", str(module / "vendor/prboom/include"),
                            str(ROOT / "tests/host/native_foreground_keys_test.c"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_cooperative_checkpoints_preserve_timed_waits(self):
        source = (ROOT / "src/services/solar_os_native_foreground.c").read_text()
        interval = re.search(r"^#define FOREGROUND_COOPERATE_INTERVAL_US .*", source, re.M).group()
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "foreground_cooperate.inc").write_text(interval + '\n' +
                production_functions(source, ("foreground_sleep_ms", "foreground_cooperate")))
            binary = Path(tmp, "cooperate")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", tmp, "-I", str(ROOT / "include"),
                            str(ROOT / "tests/host/native_foreground_cooperate_test.c"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_isolated_session_lifecycle(self):
        source = (ROOT / "src/services/solar_os_native_foreground.c").read_text()
        names = ("foreground_current", "foreground_context_state", "foreground_caller",
                 "foreground_result", "foreground_video_dimensions", "foreground_enable_suspend", "foreground_suspend_requested",
                 "foreground_guest_suspend", "foreground_get_service", "foreground_worker_cleanup",
                 "solar_os_native_foreground_entry", "solar_os_native_foreground_owner",
                 "solar_os_native_foreground_claim", "solar_os_native_foreground_release",
                 "foreground_admit", "foreground_request_stop", "foreground_stop", "foreground_suspend", "foreground_resume",
                 "foreground_read_pointer", "foreground_queue_pointer")
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "foreground.inc").write_text(production_functions(source, names))
            binary = Path(tmp, "lifecycle")
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-I", tmp, "-I", str(ROOT / "include"),
                            "-I", str(ROOT / "tests/host"),
                            str(ROOT / "tests/host/native_foreground_test.c"),
                            "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)
