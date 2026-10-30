from pathlib import Path
import re
import tempfile
import subprocess
import unittest

from tests.test_foreground_presenter import production_functions
from scripts.solaros_board_manifest import load_board_manifest

ROOT = Path(__file__).resolve().parents[1]


class SleepWakeTest(unittest.TestCase):
    def test_keyboard_interrupt_and_dedicated_key_sleep_paths(self):
        source = (ROOT / "src/main.c").read_text()
        macros = source[source.index("#ifdef SOLAR_OS_BOARD_PIN_SLEEP_WAKE"):
                        source.index("#define BLE_SLEEP_DISCONNECT_TIMEOUT_MS")]
        functions = production_functions(source, (
            "key_button_configure_gpio", "prepare_rtc_wake_gpio",
            "sleep_wake_configure_gpio", "sleep_wake_restore_gpio",
            "wait_sleep_wake_released_stable", "enter_suspend",
            "maybe_exit_suspend_for_input", "enter_sleep",
        ))
        for board_id in ("cl_32", "cl32_epd", "solar_term"):
            with self.subTest(board=board_id), tempfile.TemporaryDirectory() as tmp:
                board = load_board_manifest(ROOT / f"boards/manifests/{board_id}.toml")
                names = ("SOLAR_OS_BOARD_PIN_SLEEP_WAKE", "SOLAR_OS_BOARD_SLEEP_WAKE_ACTIVE_LEVEL",
                         "SOLAR_OS_BOARD_SLEEP_WAKE_PULL_UP", "SOLAR_OS_BOARD_SLEEP_WAKE_PULL_DOWN",
                         "SOLAR_OS_BOARD_PIN_KEY", "SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL",
                         "SOLAR_OS_BOARD_KEY_PULL_UP", "SOLAR_OS_BOARD_KEY_PULL_DOWN")
                defines = {name: "0" for name in names if "SLEEP_WAKE" not in name}
                defines.update({name: value for name, value in board["defines"].items() if name in names})
                defines["SOLAR_OS_BOARD_HAS_KEY"] = str(int("key" in board["build"]["capabilities"]))
                defines["SOLAR_OS_BOARD_HAS_WIRED_KEYBOARD"] = str(int(board_id in ("cl_32", "cl32_epd")))
                preamble = "\n".join(f"#define {name} {value}" for name, value in defines.items())
                Path(tmp, "sleep.inc").write_text(preamble + "\n" + macros + "\n" + functions)
                binary = Path(tmp, "sleep")
                subprocess.run([
                    "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                    "-I", tmp, "-I", str(ROOT / "tests/host"),
                    "-I", str(ROOT / "src/services"),
                    str(ROOT / "tests/host/sleep_wake_test.c"), "-o", str(binary),
                ], check=True)
                subprocess.run([str(binary)], check=True)

    def test_cl32_shared_interrupt_acknowledgement(self):
        source = (ROOT / "src/services/solar_os_cl32_core.c").read_text()
        defines = "\n".join(re.findall(r"^#define CL32_CORE_.*$", source, re.M))
        functions = production_functions(source, ("acknowledge_core_interrupts", "poll_keyboard"))
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "cl32_interrupt.inc").write_text(defines + "\n" + functions)
            binary = Path(tmp, "interrupt")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                str(ROOT / "tests/host/cl32_interrupt_test.c"), "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)
