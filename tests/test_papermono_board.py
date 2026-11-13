from pathlib import Path
import re
import tomllib
import unittest

from tests.test_shutdown_policy import function, run_c

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "boards/manifests/m5stack_papermono.toml"


class PaperMonoBoardTest(unittest.TestCase):
    def test_button_b_is_the_system_key_with_clean_shutdown(self):
        board = tomllib.loads(MANIFEST.read_text())
        defines = board["defines"]
        self.assertIn("key", board["build"]["capabilities"])
        self.assertEqual(defines["SOLAR_OS_BOARD_PIN_KEY"], "GPIO_NUM_3")
        self.assertEqual(defines["SOLAR_OS_BOARD_KEY_ACTIVE_LEVEL"], "0")
        self.assertEqual(defines["SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY"], "SOLAR_OS_KEY_ESCAPE")
        self.assertEqual(defines["SOLAR_OS_BOARD_KEY_LONG_ACTION"],
                         "SOLAR_OS_POWER_KEY_ACTION_SHUTDOWN")
        # Button B is the KEY only; the buttons service keeps button A.
        self.assertNotIn("GPIO_NUM_3", defines["SOLAR_OS_BOARD_BUTTONS"])
        self.assertIn("GPIO_NUM_2", defines["SOLAR_OS_BOARD_BUTTONS"])
        self.assertEqual(defines["SOLAR_OS_BOARD_PIN_BLE_PAIR"], "GPIO_NUM_2")
        self.assertEqual(defines["SOLAR_OS_BOARD_PIN_SLEEP_WAKE"], "GPIO_NUM_2")
        self.assertEqual(defines["SOLAR_OS_BOARD_POWEROFF_PROVIDER"], '"pmic0"')

        # The KEY long press and the shell `shutdown` share one request path.
        main = (ROOT / "src/main.c").read_text()
        self.assertRegex(main, r"case SOLAR_OS_POWER_KEY_ACTION_SHUTDOWN: \{\n\s+const esp_err_t err = "
                               r"solar_os_shutdown_request\(\);")
        self.assertIn("solar_os_shutdown_request()",
                      (ROOT / "src/shell/solar_os_shell_system.c").read_text())

        source = r'''
#include <assert.h>
#include <stdbool.h>
#include "solar_os_keys.h"
#include "solar_os_power.h"
#define SOLAR_OS_BOARD_KEY_SHORT_INPUT_KEY SOLAR_OS_KEY_ESCAPE
#define SOLAR_OS_BOARD_KEY_LONG_ACTION SOLAR_OS_POWER_KEY_ACTION_SHUTDOWN
'''
        power = (ROOT / "src/services/solar_os_power.c").read_text()
        match = re.search(r"^static solar_os_power_key_action_t default_key_action\(bool long_press\)\n\{.*?^\}",
                          power, re.M | re.S)
        self.assertIsNotNone(match)
        source += match.group() + r'''
int main(void) {
    assert(default_key_action(false) == SOLAR_OS_POWER_KEY_ACTION_DEFAULT);
    assert(default_key_action(true) == SOLAR_OS_POWER_KEY_ACTION_SHUTDOWN);
    return 0;
}
'''
        run_c(source, "-I", str(ROOT / "src/services"), "-I", str(ROOT / "include"))


if __name__ == "__main__":
    unittest.main()
