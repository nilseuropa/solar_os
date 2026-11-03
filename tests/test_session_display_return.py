"""Exercise display-app return routing with the production session functions."""
from pathlib import Path
import subprocess
import tempfile
import unittest

from test_foreground_presenter import production_functions

ROOT = Path(__file__).resolve().parents[1]


class SessionDisplayReturnTest(unittest.TestCase):
    def test_launch_exit_and_keyboard_return_to_own_display(self):
        source = (ROOT / "src/services/solar_os_sessions.c").read_text()
        functions = production_functions(source, (
            "session_by_id", "session_return_target", "session_return_shell",
            "session_effective_gfx", "session_bind_display", "session_uses_same_display",
            "switch_to_display_app", "close_session", "session_active_for_display",
            "solar_os_sessions_dispatch_input_event"))
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "session_display_return.inc").write_text(functions)
            binary = Path(tmp, "session_return")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                str(ROOT / "tests/host/session_display_return_test.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
