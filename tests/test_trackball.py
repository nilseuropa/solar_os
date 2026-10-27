from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

ROOT = Path(__file__).resolve().parents[1]


class TrackballTest(unittest.TestCase):
    def test_driver_capture_and_navigation_lifecycle(self):
        board = tomllib.loads((ROOT / "boards/manifests/t_deck_plus.toml").read_text())
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "solar_os_board.h").write_text(
                '#pragma once\n#include "solar_os_keys.h"\n'
                '#include "solar_os_input_composition.h"\n'
                '#define SOLAR_OS_BOARD_INPUT_CHORDS ' +
                board["defines"]["SOLAR_OS_BOARD_INPUT_CHORDS"] + '\n')
            binary = Path(tmp, "trackball")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I", tmp, "-I", str(ROOT / "tests/host/gpio_controller_stubs"),
                "-I", str(ROOT / "tests/host"), "-I", str(ROOT / "src"),
                "-I", str(ROOT / "src/services"), "-I", str(ROOT / "src/jobs"),
                "-I", str(ROOT / "src/shell"), "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/trackball_test.c"),
                str(ROOT / "src/services/solar_os_input.c"),
                str(ROOT / "src/services/solar_os_input_composition.c"),
                str(ROOT / "src/services/solar_os_keys.c"),
                str(ROOT / "src/services/solar_os_trackball.c"),
                str(ROOT / "src/jobs/solar_os_pointer_arrows_job.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
