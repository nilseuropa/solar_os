from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class StreamEchoTest(unittest.TestCase):
    def test_command_with_real_stream_registry(self):
        source = (ROOT / "src/shell/solar_os_shell_stream_echo.c").read_text()
        source = "\n".join(line for line in source.splitlines() if not line.startswith("#include"))
        fixture = (ROOT / "tests/host/stream_echo_test.inc").read_text().replace("/* COMMAND */", source)
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory)
            (path / "test.c").write_text(fixture)
            result = subprocess.run([
                "cc", "-std=c11", "-D_GNU_SOURCE", "-Wall", "-Wextra", "-Werror",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I" + str(ROOT / "tests/host"), "-I" + str(ROOT / "src/services"),
                str(path / "test.c"), str(ROOT / "src/services/solar_os_stream.c"),
                "-lm", "-o", str(path / "test")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)
            subprocess.run([str(path / "test")], check=True)
