from pathlib import Path
import subprocess
import tempfile
import tomllib
import unittest

from tests.test_foreground_presenter import production_functions
from tests.test_shutdown_policy import run_c

ROOT = Path(__file__).resolve().parents[1]


class TdeckKeyboardTest(unittest.TestCase):
    def test_suspend_preserves_ble_reconnect_wake(self):
        wake = production_functions((ROOT / "src/main.c").read_text(),
                                    ("maybe_exit_suspend_for_input",))
        run_c(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#define SOLAR_OS_PACKAGE_SERVICE_BLE 1
#define SOLAR_OS_BOARD_CAP_BLE 1
typedef struct { bool suspend_active; } solar_os_power_status_t;
static bool suspended, ble_enabled = true, ble_capable = true;
static uint32_t input_generation, ble_generation;
static uint32_t suspend_input_key_press_generation, suspend_ble_wake_generation;
static unsigned resumes;
static void solar_os_power_get_status(solar_os_power_status_t *s)
{ s->suspend_active = suspended; }
static bool board_has(int capability)
{ assert(capability == SOLAR_OS_BOARD_CAP_BLE); return ble_capable; }
static bool solar_os_ble_keyboard_enabled_for_current_boot(void) { return ble_enabled; }
static uint32_t solar_os_ble_keyboard_wake_generation(void) { return ble_generation; }
static uint32_t solar_os_input_key_press_generation(void) { return input_generation; }
static void exit_suspend(const char *reason)
{ assert(suspended && reason != NULL); suspended = false; resumes++; }
''' + wake + r'''
int main(void) {
    /* Reconnect can wake without submitting a key to the common service. */
    suspended = true;
    maybe_exit_suspend_for_input(); assert(suspended && resumes == 0);
    ble_generation++;
    maybe_exit_suspend_for_input(); assert(!suspended && resumes == 1);
    maybe_exit_suspend_for_input(); assert(resumes == 1);
    suspended = true;
    maybe_exit_suspend_for_input(); assert(suspended);
    ble_enabled = false; ble_generation++;
    maybe_exit_suspend_for_input(); assert(suspended);
    /* Local keyboard wake must work while BLE is disabled or unavailable. */
    input_generation++;
    maybe_exit_suspend_for_input(); assert(!suspended && resumes == 2);
    suspend_input_key_press_generation = input_generation;
    suspended = true; ble_capable = false; ble_enabled = true;
    maybe_exit_suspend_for_input(); assert(suspended);
    input_generation++;
    maybe_exit_suspend_for_input(); assert(!suspended && resumes == 3);
}
''')

    def test_shortcuts_preserve_modifiers_and_toggle_backlight(self):
        source = (ROOT / "src/services/solar_os_tdeck_keyboard.c").read_text()
        defines = tomllib.loads((ROOT / "boards/manifests/t_deck_plus.toml").read_text())["defines"]
        with tempfile.TemporaryDirectory() as tmp:
            Path(tmp, "tdeck_matrix.inc").write_text(production_functions(source, (
                "key_usage", "process_matrix", "reset_matrix", "poll_matrix")))
            Path(tmp, "suspend_wake.inc").write_text(production_functions(
                (ROOT / "src/main.c").read_text(), ("maybe_exit_suspend_for_input",)))
            Path(tmp, "solar_os_board.h").write_text('#pragma once\n#include "solar_os_keys.h"\n' + ''.join(
                f'#define {name} {defines[name]}\n' for name in (
                    "SOLAR_OS_BOARD_TDECK_KEYMAP", "SOLAR_OS_BOARD_INPUT_CHORDS")))
            binary = Path(tmp, "keyboard")
            subprocess.run([
                "cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                "-D_POSIX_C_SOURCE=200809L",
                "-ffunction-sections", "-fdata-sections", "-Wl,--gc-sections",
                "-include", str(ROOT / "tests/host/compat.h"),
                "-I", tmp, "-I", str(ROOT / "tests/host"),
                "-I", str(ROOT / "src"), "-I", str(ROOT / "src/services"),
                "-I", str(ROOT / "src/shell"),
                "-idirafter", str(ROOT / "include"),
                str(ROOT / "tests/host/tdeck_keyboard_test.c"),
                str(ROOT / "src/services/solar_os_input.c"),
                str(ROOT / "src/services/solar_os_input_composition.c"),
                str(ROOT / "src/services/solar_os_keys.c"),
                str(ROOT / "src/services/solar_os_tui_widgets.c"),
                "-o", str(binary),
            ], check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
